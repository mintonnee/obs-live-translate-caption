#include "caption-pipeline.hpp"
#include "translate-protocol.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>

using namespace lt;

namespace {
struct Fixture {
    CaptionPipeline pipeline;
    CaptionPipelineConfig config;
    uint64_t now = 0;
    std::vector<TranslationJobPtr> blocked;
    Fixture() { REQUIRE(pipeline.reset(1, config, now).accepted); }
    TranslationJobPtr send() {
        auto job = pipeline.dispatch(now).job;
        REQUIRE(job);
        blocked.push_back(job);
        return job;
    }
    CaptionPipelineResult finish(const TranslationJobPtr &job, const std::string &text,
                                 TranslationFailure failure = TranslationFailure::None) {
        return pipeline.complete({job->id, failure == TranslationFailure::None
            ? TranslationOutcome::Success : TranslationOutcome::Failed, failure, text, 0, now}, now);
    }
    std::string frame() { return pipeline.tick(now).snapshot.text; }
    void stable(const std::string &prefix) {
        pipeline.interim(prefix + " a", now);
        pipeline.interim(prefix + " b", now + 400);
        now += 800;
        pipeline.interim(prefix + " c", now);
    }
};
}

TEST_CASE("pipeline barrier responses C A B append in order without skipping")
{
    Fixture f;
    f.pipeline.final("Alpha.", 0);
    f.pipeline.final("Beta.", 0);
    f.pipeline.final("Gamma.", 0);
    auto a = f.send(), b = f.send(), c = f.send();
    REQUIRE_FALSE(f.pipeline.dispatch(0).job);
    f.finish(c, "C");
    REQUIRE(f.frame().empty());
    f.finish(a, "A");
    f.finish(b, "B");
    REQUIRE(f.frame() == "A");
    f.now = 1199;
    REQUIRE(f.frame() == "A");
    f.now = 1200;
    REQUIRE(f.frame() == "A B");
    f.now = 2400;
    REQUIRE(f.frame() == "A B C");
    REQUIRE(f.pipeline.in_flight_count() == 0);
}

TEST_CASE("pipeline stable interim final confirmation does not duplicate HTTP")
{
    Fixture f;
    f.stable("The original sentence.");
    REQUIRE(f.pipeline.queued_count() == 1);
    auto initial = f.send();
    f.pipeline.final("The original sentence. Tail.", 900);
    f.now = 900;
    REQUIRE(f.pipeline.queued_count() == 1);
    auto tail = f.send();
    REQUIRE(initial->id.segment != tail->id.segment);
    REQUIRE(tail->context == std::vector<std::string>{"The original sentence."});
    REQUIRE_FALSE(f.pipeline.dispatch(900).job);
}

TEST_CASE("pipeline unchanged final gives the visible interim a full hold after speech ends")
{
    Fixture f;
    f.stable("The original sentence.");
    auto initial = f.send();
    f.now = 1000;
    REQUIRE(f.finish(initial, "translated").accepted);
    REQUIRE(f.frame() == "translated");
    REQUIRE(f.pipeline.snapshot().display.page_expires_ms == 5000);

    f.now = 4900;
    REQUIRE(f.pipeline.final("The original sentence.", f.now).accepted);
    REQUIRE_FALSE(f.pipeline.dispatch(f.now).job);
    REQUIRE(f.frame() == "translated");
    REQUIRE(f.pipeline.snapshot().display.page_expires_ms == 8900);
    f.now = 5000;
    REQUIRE(f.frame() == "translated");
    f.now = 8899;
    REQUIRE(f.frame() == "translated");
    f.now = 8900;
    REQUIRE(f.frame().empty());
}

TEST_CASE("pipeline revised final keeps the visible interim while its translation is pending")
{
    Fixture f;
    f.stable("The original sentence.");
    auto initial = f.send();
    f.now = 1000;
    f.finish(initial, "interim translation");
    REQUIRE(f.frame() == "interim translation");
    f.now = 4900;
    f.pipeline.final("The revised sentence.", f.now);
    auto revision = f.send();
    REQUIRE(f.frame() == "interim translation");
    f.now = 5000;
    REQUIRE(f.frame() == "interim translation");
    f.now = 5500;
    REQUIRE(f.finish(revision, "final translation").accepted);
    REQUIRE(f.frame() == "final translation");
    REQUIRE(f.pipeline.snapshot().display.page_expires_ms == 9500);
}

TEST_CASE("pipeline ambiguous final preserves the visible window and lets a new utterance follow")
{
    Fixture f;
    f.stable("First sentence.");
    auto first = f.send();
    f.finish(first, "first");
    REQUIRE(f.frame() == "first");
    f.now = 1000;
    f.stable("First sentence. Second sentence.");
    auto second = f.send();
    f.finish(second, "second");
    f.now = 2000;
    REQUIRE(f.frame() == "first second");
    REQUIRE(f.pipeline.find(first->id.segment)->display == CaptionDisplayState::Retired);

    f.now = 3500;
    const auto finalized = f.pipeline.final("First sentence and second sentence.", f.now);
    REQUIRE(std::any_of(finalized.events.begin(), finalized.events.end(), [](const auto &event) {
        return event.kind == CaptionPipelineEventKind::ReconcileAmbiguous;
    }));
    REQUIRE(f.frame() == "first second");
    REQUIRE_FALSE(f.pipeline.dispatch(f.now).job);
    REQUIRE_FALSE(f.finish(second, "late duplicate").accepted);
    SECTION("hold ends from final reception") {
        f.now = 7499;
        REQUIRE(f.frame() == "first second");
        f.now = 7500;
        REQUIRE(f.frame().empty());
    }
    SECTION("next utterance does not wait for the old hold") {
        f.now = 3600;
        f.pipeline.final("Next sentence.", f.now);
        auto next = f.send();
        f.finish(next, "next");
        REQUIRE(f.frame() == "first second next");
    }
}

TEST_CASE("pipeline ambiguous final invalidating a pending replacement does not block the next utterance")
{
    Fixture f;
    f.stable("First sentence.");
    auto first = f.send();
    f.finish(first, "first");
    f.frame();
    f.now = 1000;
    f.stable("First sentence. Second sentence.");
    auto second = f.send();
    f.finish(second, "second");
    f.now = 2000;
    REQUIRE(f.frame() == "first second");
    f.now = 2100;
    f.stable("First sentence. Entirely altered tail!");
    auto replacement = f.send();
    REQUIRE(replacement->id.segment != second->id.segment);
    REQUIRE(f.frame() == "first second");
    f.now = 3500;
    f.pipeline.final("First sentence and the changed tail.", f.now);
    REQUIRE(f.frame() == "first second");
    REQUIRE_FALSE(f.finish(replacement, "invalidated").accepted);
    f.now = 3600;
    f.pipeline.final("Next sentence.", f.now);
    auto next = f.send();
    f.finish(next, "next");
    REQUIRE(f.frame() == "first second next");
}

TEST_CASE("pipeline final only disables interim jobs not segmentation and pages")
{
    Fixture f;
    f.config.incremental = false;
    f.config.display = {2, 10, 4000};
    f.pipeline.reset(2, f.config, 0);
    f.stable("The original sentence.");
    REQUIRE(f.pipeline.queued_count() == 0);
    f.pipeline.final(std::string(120, 'a') + " Tail.", f.now);
    REQUIRE(f.pipeline.queued_count() == 2);
    auto job = f.send();
    f.finish(job, "1111111111\n2222222222\n3333333333\n4444444444");
    REQUIRE(f.frame() == "1111111111\n2222222222");
    f.now += 1200;
    REQUIRE(f.frame() == "3333333333\n4444444444");
}

TEST_CASE("pipeline revision and retry barriers ignore stale and duplicate auth")
{
    Fixture f;
    f.stable("The original sentence.");
    auto old = f.send();
    f.pipeline.final("The revised sentence.", 900);
    f.now = 900;
    auto current = f.send();
    REQUIRE(current->id.segment == old->id.segment);
    REQUIRE(current->id.source_revision == 2);
    REQUIRE(f.finish(current, "current").accepted);
    REQUIRE_FALSE(f.finish(old, "", TranslationFailure::Auth).auth_error);
    REQUIRE(f.frame() == "current");
    REQUIRE(f.pipeline.request_quality_retry(current->id.segment, 1000).accepted);
    f.now = 1000;
    auto retry = f.send();
    REQUIRE(retry->id.attempt_id == 2);
    REQUIRE(f.frame() == "current");
    REQUIRE_FALSE(f.finish(current, "", TranslationFailure::Auth).auth_error);
    REQUIRE(f.finish(retry, "better").accepted);
    REQUIRE(f.frame() == "better");
    REQUIRE_FALSE(f.finish(retry, "wrong").accepted);
}

TEST_CASE("pipeline reset cancellation preserves physical slots and immutable settings")
{
    Fixture f;
    f.pipeline.final("Alpha.", 0);
    f.pipeline.final("Beta.", 0);
    f.pipeline.final("Gamma.", 0);
    auto a = f.send(), b = f.send(), c = f.send();
    f.config.translation.model = "new-model";
    auto reset = f.pipeline.reset(2, f.config, 10);
    REQUIRE(reset.cancel.size() == 3);
    REQUIRE(f.pipeline.in_flight_count() == 3);
    f.pipeline.final("New segment.", 11);
    f.now = 11;
    REQUIRE_FALSE(f.pipeline.dispatch(11).job);
    REQUIRE_FALSE(f.finish(a, "", TranslationFailure::Auth).auth_error);
    REQUIRE(f.pipeline.in_flight_count() == 2);
    auto next = f.send();
    REQUIRE(next->settings.model == "new-model");
    REQUIRE(a->settings.model.empty());
    REQUIRE(next->id.segment.segment_id > c->id.segment.segment_id);
    REQUIRE_FALSE(f.finish(b, "old").accepted);
    REQUIRE_FALSE(f.finish(c, "old").accepted);
    REQUIRE(f.frame().empty());
}

TEST_CASE("pipeline stale deadline is first registration despite a source revision")
{
    Fixture f;
    f.stable("The original sentence.");
    auto old = f.send();
    f.pipeline.final("The revised sentence.", 6000);
    auto effects = f.pipeline.tick(6800).result;
    REQUIRE(f.pipeline.queued_count() == 0);
    REQUIRE_FALSE(f.pipeline.dispatch(6800).job);
    REQUIRE(std::any_of(effects.events.begin(), effects.events.end(), [](const auto &event) {
        return event.retire_reason == CaptionRetireReason::StaleBeforeDisplay;
    }));
    f.now = 6800;
    REQUIRE_FALSE(f.finish(old, "late").accepted);
    REQUIRE(f.frame().empty());
}

TEST_CASE("pipeline one thousand synthetic finals stay bounded and report drops")
{
    Fixture f;
    size_t drops = 0;
    for (int i = 0; i < 1000; ++i) {
        auto changes = f.pipeline.final("Synthetic sentence " + std::to_string(i) + ".", 0);
        drops += std::count_if(changes.events.begin(), changes.events.end(), [](const auto &event) {
            return event.kind == CaptionPipelineEventKind::Retired;
        });
        if (i < 3) f.send();
        REQUIRE(f.pipeline.managed_count() <= 256);
        REQUIRE(f.pipeline.queued_count() <= 12);
        REQUIRE(f.pipeline.waiting_count() <= 24);
        REQUIRE(f.pipeline.in_flight_count() <= 3);
        REQUIRE(f.pipeline.source_snapshot_bytes() <= 3 * 32768);
    }
    REQUIRE(drops > 900);
    REQUIRE(f.pipeline.in_flight_count() == 3);
}

TEST_CASE("pipeline source and result limits reject without revival")
{
    Fixture f;
    auto oversized = f.pipeline.interim(std::string(32769, 'x'), 0);
    REQUIRE(std::any_of(oversized.events.begin(), oversized.events.end(), [](const auto &event) {
        return event.kind == CaptionPipelineEventKind::SourceLimit;
    }));
    f.pipeline.final("Fine.", 1);
    auto job = f.send();
    REQUIRE_FALSE(f.finish(job, std::string(16385, 'x')).accepted);
    REQUIRE_FALSE(f.finish(job, "late").accepted);
    REQUIRE(f.frame().empty());
}

TEST_CASE("pipeline display changes retain generation and publication rejects reverse delivery")
{
    Fixture f;
    f.pipeline.final("One.", 0);
    auto job = f.send();
    f.finish(job, "one");
    auto first = f.pipeline.tick(0);
    f.pipeline.set_display_config({1, 20, 1000}, 100);
    REQUIRE(f.pipeline.generation() == 1);
    auto cleared = f.pipeline.tick(1000);
    REQUIRE(cleared.snapshot.text.empty());
    CaptionPublicationGate sink;
    REQUIRE(sink.claim(cleared.snapshot.publication));
    REQUIRE_FALSE(sink.claim(first.snapshot.publication));
    REQUIRE_FALSE(sink.claim(cleared.snapshot.publication));
    REQUIRE_FALSE(sink.claim(0));
}

TEST_CASE("pipeline one to many replacement is atomic and late sibling cannot overtake")
{
    Fixture f;
    f.stable("An original sentence.");
    auto old = f.send();
    f.finish(old, "old");
    REQUIRE(f.frame() == "old");
    f.pipeline.final(std::string(119, 'a') + ". " + std::string(80, 'b') + ".", 900);
    f.now = 900;
    auto head = f.send(), tail = f.send();
    REQUIRE(head->id.segment != old->id.segment);
    f.finish(tail, "tail");
    REQUIRE(f.frame() == "old");
    f.finish(head, "head");
    REQUIRE(f.frame() == "head");
    f.now = 2099;
    REQUIRE(f.frame() == "head");
    f.now = 2100; // The replacement gets its own full minimum and hold interval.
    REQUIRE(f.frame() == "head tail");
    REQUIRE_FALSE(f.finish(old, "late").accepted);
}

TEST_CASE("pipeline many to one replacement suppresses every old request")
{
    Fixture f;
    f.stable("First sentence.");
    auto first = f.send();
    f.now = 1000;
    f.stable("First sentence. Second sentence.");
    auto second = f.send();
    f.finish(first, "first");
    REQUIRE(f.frame() == "first");
    f.pipeline.final("First sentence and second sentence.", 2000);
    f.now = 2000;
    auto merged = f.send();
    REQUIRE_FALSE(f.finish(second, "", TranslationFailure::Auth).auth_error);
    REQUIRE(f.frame() == "first");
    f.finish(merged, "merged");
    REQUIRE(f.frame() == "merged");
    REQUIRE(f.pipeline.queued_count() == 0);
}

TEST_CASE("pipeline expiry wins a simultaneous correction or auth completion")
{
    Fixture f;
    f.stable("The original sentence.");
    auto first = f.send();
    f.finish(first, "old");
    REQUIRE(f.frame() == "old");
    f.pipeline.final("The revised sentence.", 900);
    f.now = 900;
    auto revision = f.send();
    f.now = 4900; // Final at 900 refreshes the old snapshot while the revision runs.
    auto rejected = f.finish(revision, "", TranslationFailure::Auth);
    REQUIRE_FALSE(rejected.accepted);
    REQUIRE_FALSE(rejected.auth_error);
    REQUIRE(f.frame().empty());
    REQUIRE_FALSE(f.finish(revision, "resurrection").accepted);
}

TEST_CASE("pipeline final for a different utterance does not prolong the old caption")
{
    Fixture f;
    f.pipeline.final("First sentence.", 0);
    auto first = f.send();
    f.finish(first, "first");
    REQUIRE(f.frame() == "first");
    f.now = 3900;
    f.pipeline.final("Next sentence.", f.now);
    auto next = f.send();
    REQUIRE(f.frame() == "first");
    f.now = 4000;
    REQUIRE(f.frame().empty());
    f.now = 4100;
    f.finish(next, "next");
    REQUIRE(f.frame() == "next");
}

TEST_CASE("pipeline final cannot revive a caption whose hold already expired")
{
    Fixture f;
    f.stable("The original sentence.");
    auto initial = f.send();
    f.finish(initial, "interim");
    REQUIRE(f.frame() == "interim");
    f.now = 4800;
    f.pipeline.final("The original sentence.", f.now);
    REQUIRE(f.frame().empty());
    REQUIRE_FALSE(f.pipeline.dispatch(f.now).job);
}

TEST_CASE("pipeline current auth failure is distinct from stale auth")
{
    Fixture f;
    f.pipeline.final("Current.", 0);
    auto job = f.send();
    REQUIRE(f.finish(job, "", TranslationFailure::Auth).auth_error);
    REQUIRE_FALSE(f.finish(job, "", TranslationFailure::Auth).auth_error);
}

TEST_CASE("pipeline waiting initial makes progress after one visible update")
{
    Fixture f;
    f.stable("The original sentence.");
    auto first = f.send();
    f.finish(first, "old");
    f.frame();
    f.pipeline.final("The revised sentence.", 900);
    f.pipeline.final("Waiting initial.", 900);
    f.now = 900;
    auto update = f.send();
    REQUIRE(update->request_kind == TranslationRequestKind::SourceUpdate);
    auto initial = f.send();
    REQUIRE(initial->request_kind == TranslationRequestKind::Initial);
}

TEST_CASE("pipeline context uses the latest earlier logical source at most three")
{
    Fixture f;
    f.stable("The original sentence.");
    f.pipeline.final("The revised sentence.", 900);
    f.pipeline.final("Second.", 1000);
    f.pipeline.final("Third.", 1000);
    f.pipeline.final("Fourth.", 1000);
    f.now = 1000;
    auto revised = f.send();
    f.finish(revised, "revised");
    auto second = f.send();
    REQUIRE(second->context == std::vector<std::string>{"The revised sentence."});
    auto third = f.send();
    f.finish(second, "second");
    auto fourth = f.send();
    REQUIRE(fourth->context == std::vector<std::string>{"The revised sentence.", "Second.", "Third."});
    size_t bytes = 0;
    for (const auto &context : fourth->context) bytes += context.size();
    REQUIRE(bytes <= 2048);
}

TEST_CASE("pipeline event latencies distinguish registration HTTP and ready waiting")
{
    Fixture f;
    f.pipeline.interim("Sentence. a", 0);
    f.pipeline.interim("Sentence. b", 400);
    f.pipeline.interim("Sentence. c", 800);
    auto dispatch = f.pipeline.dispatch(1000);
    REQUIRE(dispatch.job);
    auto dispatched = std::find_if(dispatch.result.events.begin(), dispatch.result.events.end(),
        [](const auto &event) { return event.kind == CaptionPipelineEventKind::Dispatched; });
    REQUIRE(dispatched != dispatch.result.events.end());
    REQUIRE(dispatched->segment_wait_ms == 800);
    REQUIRE(dispatched->queue_wait_ms == 200);
    auto complete = f.pipeline.complete({dispatch.job->id, TranslationOutcome::Success,
        TranslationFailure::None, "ready", 1000, 1100}, 1100);
    REQUIRE(complete.events.back().http_ms == 100);
    auto shown = f.pipeline.tick(1400);
    auto entered = std::find_if(shown.result.events.begin(), shown.result.events.end(),
        [](const auto &event) { return event.kind == CaptionPipelineEventKind::PageEntered; });
    REQUIRE(entered != shown.result.events.end());
    REQUIRE(entered->display_wait_ms == 300);
    REQUIRE(entered->first_seen_to_display_ms == 1400);
}

TEST_CASE("pipeline long final flood releases retired metadata for the next utterance")
{
    Fixture f;
    f.pipeline.final(std::string(32768, 'a'), 0);
    REQUIRE(f.pipeline.managed_count() <= 256);
    REQUIRE(f.pipeline.queued_count() <= 12);
    f.pipeline.tick(6000);
    REQUIRE(f.pipeline.queued_count() == 0);
    f.pipeline.final("Fresh utterance.", 6001);
    auto fresh = f.pipeline.dispatch(6001).job;
    REQUIRE(fresh);
    REQUIRE(fresh->source_text == "Fresh utterance.");
}

TEST_CASE("incremental disable preserves visible provisional and cancels all unseen states")
{
    Fixture f;
    f.stable("First sentence.");
    auto visible = f.send();
    f.finish(visible, "visible");
    REQUIRE(f.frame() == "visible");
    const auto before = f.pipeline.snapshot();

    f.now = 1000;
    f.stable("First sentence. Second sentence.");
    auto ready = f.send();
    f.finish(ready, "ready but unseen");
    f.now = 2000;
    f.stable("First sentence. Second sentence. Third sentence.");
    auto running = f.send();
    f.now = 3000;
    f.stable("First sentence. Second sentence. Third sentence. Fourth sentence.");
    const auto queued_key = f.pipeline.segments().back().key;
    REQUIRE(f.pipeline.queued_count() == 1);

    const auto disabled = f.pipeline.set_incremental(false, f.now);
    REQUIRE(disabled.accepted);
    REQUIRE(disabled.cancel == std::vector<TranslationJobId>{running->id});
    REQUIRE(f.pipeline.generation() == 1);
    REQUIRE(f.pipeline.queued_count() == 0);
    REQUIRE(f.pipeline.in_flight_count() == 1); // Cancel is not confirmation.
    REQUIRE(f.pipeline.find(ready->id.segment)->display == CaptionDisplayState::Retired);
    REQUIRE(f.pipeline.find(running->id.segment)->display == CaptionDisplayState::Retired);
    REQUIRE(f.pipeline.find(queued_key)->display == CaptionDisplayState::Retired);
    REQUIRE(f.frame() == "visible");
    const auto after = f.pipeline.snapshot();
    REQUIRE(after.key == before.key);
    REQUIRE(after.display.first_visible_ms == before.display.first_visible_ms);
    REQUIRE(after.display.page_min_until_ms == before.display.page_min_until_ms);
    REQUIRE(after.display.page_expires_ms == before.display.page_expires_ms);
    REQUIRE(f.pipeline.set_incremental(false, f.now).events.empty());

    f.now = 3900;
    f.pipeline.final("First sentence. Second sentence. Third sentence. Fourth sentence. New tail.",
                     f.now);
    REQUIRE(f.pipeline.generation() == 1);
    REQUIRE(f.pipeline.queued_count() == 1);
    auto tail = f.send();
    REQUIRE(tail->source_text == "New tail.");
    REQUIRE(tail->id.segment != visible->id.segment);
    REQUIRE_FALSE(f.pipeline.dispatch(f.now).job);
    REQUIRE_FALSE(f.finish(running, "", TranslationFailure::Auth).auth_error);
    REQUIRE_FALSE(f.finish(ready, "late duplicate").accepted);
    REQUIRE(f.frame() == "visible");
    REQUIRE(f.pipeline.snapshot().display.page_expires_ms == 7900);
    f.now = 4800;
    REQUIRE(f.frame() == "visible"); // Final confirmation, not the toggle, extends the hold.
    f.now = 7900;
    REQUIRE(f.frame().empty());
}

TEST_CASE("incremental reenable needs actual distinct interim observations")
{
    Fixture f;
    REQUIRE(f.pipeline.set_incremental(false, 0).accepted);
    f.pipeline.interim("Stable sentence. a", 0);
    for (uint64_t now : {400, 800, 1200}) f.pipeline.tick(now);
    REQUIRE(f.pipeline.queued_count() == 0);
    REQUIRE(f.pipeline.set_incremental(true, 1600).accepted);
    for (uint64_t now : {2000, 2400, 2800}) f.pipeline.tick(now);
    REQUIRE(f.pipeline.queued_count() == 0);
    f.pipeline.interim("Stable sentence. b", 3000);
    REQUIRE(f.pipeline.queued_count() == 0);
    f.pipeline.interim("Stable sentence. c", 3400);
    REQUIRE(f.pipeline.queued_count() == 1);
    REQUIRE(f.pipeline.generation() == 1);
}

TEST_CASE("incremental disable does not retire finalized work from an earlier utterance")
{
    Fixture f;
    f.pipeline.final("Earlier final.", 0);
    auto earlier = f.send();
    f.stable("Current provisional.");
    REQUIRE(f.pipeline.queued_count() == 1);
    f.pipeline.set_incremental(false, f.now);
    REQUIRE(f.pipeline.queued_count() == 0);
    REQUIRE(f.pipeline.in_flight_count() == 1);
    REQUIRE(f.pipeline.find(earlier->id.segment)->display == CaptionDisplayState::Waiting);
    REQUIRE(f.finish(earlier, "earlier").accepted);
    REQUIRE(f.frame() == "earlier");
}
