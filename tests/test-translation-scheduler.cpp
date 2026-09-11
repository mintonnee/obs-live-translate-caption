#include <catch2/catch_test_macros.hpp>
#include "translation-scheduler.hpp"

#include <algorithm>
#include <limits>

using namespace lt;

namespace {

CaptionSegment make_segment(uint64_t id, uint64_t revision = 1, uint64_t generation = 1)
{
    return {{generation, 1, id}, {1, id}, revision, "source " + std::to_string(revision),
            false, {}, 7};
}

TranslationScheduler make_scheduler()
{
    TranslationScheduler scheduler;
    REQUIRE(scheduler.reset_generation(1, {"en", "English", "model-a", {"term-a"}}).accepted);
    return scheduler;
}

TranslationCompletion success(const TranslationJobPtr &job, std::string text = "translated")
{
    return {job->id, TranslationOutcome::Success, TranslationFailure::None, std::move(text), 10, 20};
}

TranslationCompletion failed(const TranslationJobPtr &job)
{
    return {job->id, TranslationOutcome::Failed, TranslationFailure::Network, {}, 10, 20};
}

// A deterministic transport barrier: starting reserves a scheduler slot; only
// release reports physical termination. No clocks, threads, sleeps or network.
struct TransportBarrier {
    TranslationScheduler scheduler = make_scheduler();
    std::vector<TranslationJobPtr> held;

    void start(uint64_t now)
    {
        while (auto job = scheduler.dispatch(now).job) held.push_back(std::move(job));
    }
    SchedulerCompletionResult release(size_t index, uint64_t now)
    {
        auto completion = success(held.at(index));
        held.erase(held.begin() + static_cast<std::ptrdiff_t>(index));
        return scheduler.complete(completion, now);
    }
};

}

TEST_CASE("three physical slots remain occupied through revision cancellation")
{
    TransportBarrier barrier;
    for (uint64_t id = 1; id <= 4; ++id)
        REQUIRE(barrier.scheduler.register_segment(make_segment(id), {}, 100).accepted);
    barrier.start(100);
    REQUIRE(barrier.held.size() == 3);
    REQUIRE(barrier.scheduler.in_flight_count() == 3);
    REQUIRE(barrier.scheduler.queued_count() == 1);

    const auto update = barrier.scheduler.register_segment(make_segment(1, 2), {}, 200);
    REQUIRE(update.effects.cancel.size() == 1);
    REQUIRE(update.effects.cancel.front() == barrier.held.front()->id);
    REQUIRE(barrier.scheduler.in_flight_count() == 3);
    REQUIRE_FALSE(barrier.scheduler.dispatch(200).job);
    REQUIRE(barrier.release(0, 300).reject == SchedulerRejectReason::StaleRevision);
    REQUIRE(barrier.scheduler.in_flight_count() == 2);
    barrier.start(300);
    REQUIRE(barrier.held.size() == 3);
    REQUIRE(barrier.held.back()->id.source_revision == 2);
}

TEST_CASE("queued revisions coalesce without resetting original timing or duplicating final")
{
    auto scheduler = make_scheduler();
    REQUIRE(scheduler.register_segment(make_segment(1), {"old context"}, 100).accepted);
    for (uint64_t revision = 2; revision <= 100; ++revision)
        REQUIRE(scheduler.register_segment(make_segment(1, revision), {"new context"}, 500).accepted);
    auto final = make_segment(1, 100);
    final.is_final = true;
    final.order_key = {0, 0};
    REQUIRE(scheduler.register_segment(final, {}, 501).reject == SchedulerRejectReason::Duplicate);
    REQUIRE(scheduler.find(final.key)->segment.is_final);
    REQUIRE(scheduler.queued_count() == 1);
    auto job = scheduler.dispatch(502).job;
    REQUIRE(job);
    REQUIRE(job->id.source_revision == 100);
    REQUIRE(job->id.attempt_id == 1);
    REQUIRE(job->first_registered_ms == 100);
    REQUIRE(job->first_seen_ms == 7);
    REQUIRE(job->queued_ms == 500);
    REQUIRE(job->context == std::vector<std::string>{"new context"});
    REQUIRE(job->order_key == final.order_key);
    REQUIRE_FALSE(scheduler.dispatch(502).job);
    auto conflict = final;
    conflict.source_text = "changed without revision";
    REQUIRE(scheduler.register_segment(conflict, {}, 503).reject ==
            SchedulerRejectReason::RevisionConflict);
}

TEST_CASE("new revision success dominates obsolete success and obsolete auth failure")
{
    auto scheduler = make_scheduler();
    scheduler.register_segment(make_segment(1), {}, 0);
    auto v1 = scheduler.dispatch(0).job;
    scheduler.register_segment(make_segment(1, 2), {}, 1);
    auto v2 = scheduler.dispatch(1).job;
    REQUIRE(scheduler.complete(success(v2, "new"), 2).accepted);
    auto old = success(v1, "old");
    SECTION("old success") {}
    SECTION("old auth failure") {
        old.outcome = TranslationOutcome::Failed;
        old.failure = TranslationFailure::Auth;
    }
    REQUIRE(scheduler.complete(old, 3).reject == SchedulerRejectReason::StaleRevision);
    REQUIRE(scheduler.find(v2->id.segment)->translated_text == "new");
    REQUIRE(scheduler.complete(success(v2), 4).reject == SchedulerRejectReason::Duplicate);
    REQUIRE(scheduler.complete(failed(v2), 5).reject == SchedulerRejectReason::Duplicate);
    REQUIRE(scheduler.find(v2->id.segment)->translated_text == "new");
    REQUIRE(scheduler.in_flight_count() == 0);
}

TEST_CASE("quality retry dominates prior attempt and a new revision resets attempt budget")
{
    auto scheduler = make_scheduler();
    scheduler.register_segment(make_segment(1, 2), {}, 10);
    auto a1 = scheduler.dispatch(10).job;
    REQUIRE(scheduler.request_quality_retry(a1->id.segment, {}, 11).accepted);
    REQUIRE(scheduler.request_quality_retry(a1->id.segment, {}, 12).reject ==
            SchedulerRejectReason::Duplicate);
    auto a2 = scheduler.dispatch(12).job;
    REQUIRE(a2->id.attempt_id == 2);
    REQUIRE(a2->request_kind == TranslationRequestKind::QualityRetry);
    REQUIRE(scheduler.complete(success(a2, "a2"), 13).accepted);
    REQUIRE(scheduler.complete(success(a1), 14).reject == SchedulerRejectReason::StaleAttempt);
    REQUIRE(scheduler.complete(failed(a1), 15).reject == SchedulerRejectReason::StaleAttempt);
    REQUIRE(scheduler.find(a2->id.segment)->translated_text == "a2");
    REQUIRE(scheduler.register_segment(make_segment(1, 3), {}, 16).accepted);
    auto v3 = scheduler.dispatch(16).job;
    REQUIRE(v3->id.attempt_id == 1);
    REQUIRE(v3->first_registered_ms == 10);
    REQUIRE(scheduler.complete(success(a2), 17).reject == SchedulerRejectReason::StaleRevision);
}

TEST_CASE("retry failure preserves visible text but initial terminal failure retires once")
{
    auto scheduler = make_scheduler();
    scheduler.register_segment(make_segment(1), {}, 0);
    auto job = scheduler.dispatch(0).job;
    SECTION("existing translation survives") {
        REQUIRE(scheduler.complete(success(job, "visible"), 1).accepted);
        REQUIRE(scheduler.mark_visible(job->id.segment, 1).accepted);
        REQUIRE(scheduler.request_quality_retry(job->id.segment, {}, 2).accepted);
        auto retry = scheduler.dispatch(2).job;
        REQUIRE(scheduler.complete(failed(retry), 3).accepted);
        REQUIRE(scheduler.find(job->id.segment)->translated_text == "visible");
        REQUIRE(scheduler.find(job->id.segment)->display == CaptionDisplayState::Visible);
        REQUIRE(scheduler.complete(success(retry), 4).reject == SchedulerRejectReason::Duplicate);
    }
    SECTION("no translation releases ordering wait") {
        const auto result = scheduler.complete(failed(job), 1);
        REQUIRE(result.accepted);
        REQUIRE(result.effects.retired.size() == 1);
        REQUIRE(result.effects.retired.front().reason == CaptionRetireReason::TranslationFailed);
        REQUIRE(scheduler.complete(failed(job), 2).effects.retired.empty());
        REQUIRE(scheduler.complete(success(job), 3).reject == SchedulerRejectReason::RetiredSegment);
    }
}

TEST_CASE("undispatched and forged completions never consume a real slot or mutate results")
{
    auto scheduler = make_scheduler();
    scheduler.register_segment(make_segment(1), {}, 0);
    const auto id = scheduler.find(make_segment(1).key)->current_job;
    TranslationCompletion completion{id, TranslationOutcome::Success,
                                     TranslationFailure::None, "unsolicited", 0, 1};
    REQUIRE(scheduler.complete(completion, 1).reject == SchedulerRejectReason::NotDispatched);
    REQUIRE_FALSE(scheduler.find(id.segment)->result_processed);
    auto job = scheduler.dispatch(2).job;
    completion.id.attempt_id = 7;
    REQUIRE(scheduler.complete(completion, 3).reject == SchedulerRejectReason::StaleAttempt);
    REQUIRE(scheduler.in_flight_count() == 1);
    REQUIRE(scheduler.complete(success(job), 4).accepted);
}

TEST_CASE("generation reset invalidates callbacks while preserving occupied transport slots")
{
    TransportBarrier barrier;
    for (uint64_t id = 1; id <= 4; ++id)
        barrier.scheduler.register_segment(make_segment(id), {}, 0);
    barrier.start(0);
    auto old_settings_job = barrier.held.front();
    TranslationSettingsSnapshot settings{"ja", "Japanese", "model-b", {"term-b"}};
    auto reset = barrier.scheduler.reset_generation(2, settings);
    REQUIRE(reset.accepted);
    REQUIRE(reset.effects.retired.size() == 4);
    REQUIRE(reset.effects.cancel.size() == 3);
    settings.model = "mutated caller settings";
    REQUIRE(barrier.scheduler.queued_count() == 0);
    REQUIRE(barrier.scheduler.managed_count() == 0);
    REQUIRE(barrier.scheduler.in_flight_count() == 3);
    REQUIRE(barrier.scheduler.register_segment(make_segment(5, 1, 2), {}, 1).accepted);
    REQUIRE_FALSE(barrier.scheduler.dispatch(1).job);
    REQUIRE(barrier.release(0, 2).reject == SchedulerRejectReason::StaleGeneration);
    auto fresh = barrier.scheduler.dispatch(2).job;
    REQUIRE(fresh);
    REQUIRE(fresh->settings.model == "model-b");
    REQUIRE(fresh->settings.target_code == "ja");
    REQUIRE(fresh->settings.glossary == std::vector<std::string>{"term-b"});
    REQUIRE(old_settings_job->settings.model == "model-a");
    auto auth = failed(barrier.held.front());
    auth.failure = TranslationFailure::Auth;
    REQUIRE_FALSE(barrier.scheduler.complete(auth, 3).accepted);
    REQUIRE(barrier.scheduler.generation() == 2);
    REQUIRE_FALSE(barrier.scheduler.reset_generation(2, {}).accepted);
    REQUIRE_FALSE(barrier.scheduler.reset_generation(1, {}).accepted);
    // Repeated stop/config/reconnect cycles cannot free a pending request.
    for (uint64_t generation = 3; generation <= 8; ++generation) {
        REQUIRE(barrier.scheduler.reset_generation(generation, {}).accepted);
        REQUIRE(barrier.scheduler.in_flight_count() == 2);
    }
    REQUIRE_FALSE(barrier.scheduler.complete(success(fresh), 10).accepted);
    REQUIRE(barrier.scheduler.in_flight_count() == 1);
}

TEST_CASE("stale deadline survives revisions retries and expires at exactly six seconds")
{
    auto scheduler = make_scheduler();
    scheduler.register_segment(make_segment(1), {}, 100);
    auto old = scheduler.dispatch(101).job;
    REQUIRE(scheduler.register_segment(make_segment(1, 2), {}, 6000).accepted);
    REQUIRE(scheduler.request_quality_retry(old->id.segment, {}, 6099).accepted);
    REQUIRE(scheduler.tick(6099).retired.empty());
    REQUIRE(scheduler.tick(50).retired.empty()); // Defensive clock regression, no underflow.
    SchedulerEffects effects;
    SECTION("tick") { effects = scheduler.tick(6100); }
    SECTION("dispatch") {
        const auto dispatch = scheduler.dispatch(6100);
        REQUIRE_FALSE(dispatch.job);
        effects = dispatch.effects;
    }
    SECTION("completion wins no race against expiry") {
        auto current = scheduler.dispatch(6099).job;
        const auto completion = scheduler.complete(success(current), 6100);
        REQUIRE(completion.reject == SchedulerRejectReason::RetiredSegment);
        effects = completion.effects;
    }
    REQUIRE(effects.retired.size() == 1);
    REQUIRE(effects.retired.front().reason == CaptionRetireReason::StaleBeforeDisplay);
    REQUIRE(scheduler.queued_count() == 0);
    REQUIRE(scheduler.in_flight_count() == 1);
    REQUIRE(scheduler.complete(success(old), 6200).reject == SchedulerRejectReason::RetiredSegment);
    REQUIRE(scheduler.in_flight_count() == 0);
}

TEST_CASE("visible segments do not acquire a new stale before display deadline")
{
    auto scheduler = make_scheduler();
    scheduler.register_segment(make_segment(1), {}, 0);
    auto first = scheduler.dispatch(0).job;
    scheduler.complete(success(first), 1);
    REQUIRE(scheduler.mark_visible(first->id.segment, 5999).accepted);
    REQUIRE(scheduler.tick(6000).retired.empty());
    REQUIRE(scheduler.register_segment(make_segment(1, 2), {}, 10000).accepted);
    auto update = scheduler.dispatch(10000).job;
    REQUIRE(update);
    REQUIRE(update->first_registered_ms == 0);
}

TEST_CASE("replacement and explicit retirement remain terminal even after metadata reclamation")
{
    auto scheduler = make_scheduler();
    scheduler.register_segment(make_segment(1), {}, 0);
    auto old = scheduler.dispatch(0).job;
    auto replacement = make_segment(2);
    replacement.order_key = {1, 0};
    replacement.replaces = {old->id.segment};
    auto result = scheduler.register_segment(replacement, {}, 1);
    REQUIRE(result.effects.retired.size() == 1);
    REQUIRE(result.effects.retired.front().reason == CaptionRetireReason::Replaced);
    REQUIRE(scheduler.find(old->id.segment)->retire_reason == CaptionRetireReason::Replaced);
    REQUIRE(scheduler.in_flight_count() == 1);
    REQUIRE(scheduler.complete(success(old), 2).reject == SchedulerRejectReason::RetiredSegment);
    REQUIRE_FALSE(scheduler.register_segment(make_segment(1, 2), {}, 3).accepted);
    REQUIRE(scheduler.forget_retired(old->id.segment));
    REQUIRE(scheduler.register_segment(make_segment(1), {}, 4).reject ==
            SchedulerRejectReason::ReusedId);
    REQUIRE(scheduler.complete(success(old), 5).reject == SchedulerRejectReason::MissingSegment);
    REQUIRE_FALSE(scheduler.forget_retired(replacement.key));
    auto fresh = scheduler.dispatch(5).job;
    scheduler.complete(success(fresh), 6);
    REQUIRE(scheduler.retire(replacement.key, CaptionRetireReason::Completed).accepted);
    REQUIRE(scheduler.find(replacement.key)->translated_text.empty());
    REQUIRE(scheduler.retire(replacement.key, CaptionRetireReason::QueueLimit).effects.retired.empty());
    REQUIRE(scheduler.find(replacement.key)->retire_reason == CaptionRetireReason::Completed);
    REQUIRE(scheduler.reset_generation(2, {}).accepted);
    REQUIRE(scheduler.register_segment(make_segment(2, 1, 2), {}, 7).reject ==
            SchedulerRejectReason::ReusedId);
}

TEST_CASE("display updates get priority with one update fairness then waiting initial")
{
    auto scheduler = make_scheduler();
    auto visible = make_segment(1);
    visible.order_key = {20, 0};
    scheduler.register_segment(visible, {}, 0);
    auto first = scheduler.dispatch(0).job;
    scheduler.complete(success(first), 1);
    scheduler.mark_visible(visible.key, 1);
    auto initial = make_segment(2);
    initial.order_key = {1, 0};
    scheduler.register_segment(initial, {}, 2);
    visible.source_revision = 2;
    visible.source_text = "v2";
    scheduler.register_segment(visible, {}, 2);
    auto update = scheduler.dispatch(2).job;
    REQUIRE(update->id.segment == visible.key);
    visible.source_revision = 3;
    visible.source_text = "v3";
    scheduler.register_segment(visible, {}, 3);
    auto next = scheduler.dispatch(3).job;
    REQUIRE(next->id.segment == initial.key);
    REQUIRE(next->request_kind == TranslationRequestKind::Initial);
    auto third = scheduler.dispatch(3).job;
    REQUIRE(third->id.segment == visible.key);
}

TEST_CASE("segment ordering uses order keys and retries have lower priority")
{
    auto scheduler = make_scheduler();
    auto a = make_segment(1);
    auto b = make_segment(2);
    auto c = make_segment(3);
    a.order_key = {3, 0};
    b.order_key = {2, 0};
    c.order_key = {1, 0};
    for (const auto &segment : {a, b, c}) scheduler.register_segment(segment, {}, 0);
    scheduler.request_quality_retry(c.key, {}, 0);
    REQUIRE(scheduler.dispatch(0).job->id.segment == b.key);
    REQUIRE(scheduler.dispatch(0).job->id.segment == a.key);
    REQUIRE(scheduler.dispatch(0).job->id.segment == c.key);
}

TEST_CASE("one thousand queued segments stay bounded with every retirement counted")
{
    auto scheduler = make_scheduler();
    size_t queue_retirements = 0;
    size_t managed_rejections = 0;
    for (uint64_t id = 1; id <= 1000; ++id) {
        const auto result = scheduler.register_segment(make_segment(id), {}, id);
        for (const auto &retired : result.effects.retired) {
            if (retired.reason == CaptionRetireReason::QueueLimit) ++queue_retirements;
            if (retired.reason == CaptionRetireReason::SegmentLimit) ++managed_rejections;
        }
        REQUIRE(scheduler.queued_count() <= 12);
        REQUIRE(scheduler.waiting_count() <= 24);
        REQUIRE(scheduler.managed_count() <= 256);
        REQUIRE(scheduler.in_flight_count() <= 3);
    }
    REQUIRE(scheduler.queued_count() == 12);
    REQUIRE(scheduler.managed_count() == 256);
    REQUIRE(queue_retirements == 244);
    REQUIRE(managed_rejections == 744);
    REQUIRE(scheduler.find(make_segment(1).key)->retire_reason == CaptionRetireReason::QueueLimit);
    REQUIRE(scheduler.forget_retired(make_segment(1).key));
    REQUIRE(scheduler.register_segment(make_segment(1001), {}, 1001).accepted);
    REQUIRE(scheduler.managed_count() == 256);
}

TEST_CASE("completed waiting segments count toward the twenty four segment limit")
{
    auto scheduler = make_scheduler();
    for (uint64_t id = 1; id <= 24; ++id) {
        scheduler.register_segment(make_segment(id), {}, 0);
        auto job = scheduler.dispatch(0).job;
        REQUIRE(scheduler.complete(success(job), 0).accepted);
    }
    REQUIRE(scheduler.waiting_count() == 24);
    const auto result = scheduler.register_segment(make_segment(25), {}, 0);
    REQUIRE(result.effects.retired.size() == 1);
    REQUIRE(result.effects.retired.front().key == make_segment(1).key);
    REQUIRE(result.effects.retired.front().reason == CaptionRetireReason::SegmentLimit);
    REQUIRE(scheduler.waiting_count() == 24);
    REQUIRE(scheduler.find(make_segment(1).key)->translated_text.empty());
}

TEST_CASE("queue overflow retires oldest unseen segment and never the visible page")
{
    auto scheduler = make_scheduler();
    scheduler.register_segment(make_segment(1), {}, 0);
    auto first = scheduler.dispatch(0).job;
    scheduler.complete(success(first), 0);
    scheduler.mark_visible(first->id.segment, 0);
    for (uint64_t id = 2; id <= 15; ++id)
        scheduler.register_segment(make_segment(id), {}, id);
    REQUIRE(scheduler.queued_count() == 12);
    REQUIRE(scheduler.find(first->id.segment)->display == CaptionDisplayState::Visible);
    REQUIRE(scheduler.find(first->id.segment)->translated_text == "translated");
    REQUIRE(scheduler.find(make_segment(2).key)->retire_reason == CaptionRetireReason::QueueLimit);
    auto second = scheduler.dispatch(20).job;
    scheduler.complete(success(second), 20);
    REQUIRE(scheduler.mark_visible(second->id.segment, 20).reject ==
            SchedulerRejectReason::DisplayConflict);
}

TEST_CASE("context drops oldest whole UTF8 entries within count and byte budgets")
{
    auto scheduler = make_scheduler();
    std::vector<std::string> context;
    SECTION("count") { context = {"discard", "one", "two", "three"}; }
    SECTION("bytes") { context = {std::string(2048, 'a'), "one", "two", "three"}; }
    scheduler.register_segment(make_segment(1), context, 0);
    auto job = scheduler.dispatch(0).job;
    REQUIRE(job->context == std::vector<std::string>{"one", "two", "three"});
    const std::string multibyte = "\xe3\x81\x82";
    std::string boundary(2045, 'a');
    boundary += multibyte;
    scheduler.register_segment(make_segment(2), {"older", boundary}, 1);
    REQUIRE(scheduler.dispatch(1).job->context == std::vector<std::string>{boundary});
    scheduler.register_segment(make_segment(3), {boundary + "a"}, 2);
    REQUIRE(scheduler.dispatch(2).job->context.empty());
}

TEST_CASE("source boundary rejects oversize and malformed UTF8 without partial slicing")
{
    auto scheduler = make_scheduler();
    auto source = make_segment(1);
    source.source_text = std::string(32765, 'a') + "\xe3\x81\x82";
    REQUIRE(scheduler.register_segment(source, {}, 0).accepted);
    auto job = scheduler.dispatch(0).job;
    REQUIRE(job->source_text.size() == 32768);
    source.source_revision = 2;
    SECTION("oversize") {
        source.source_text += "a";
        REQUIRE(scheduler.register_segment(source, {}, 1).reject == SchedulerRejectReason::SourceLimit);
    }
    SECTION("invalid UTF8") {
        source.source_text = "\xf0\x80\x80\x80";
        REQUIRE(scheduler.register_segment(source, {}, 1).reject == SchedulerRejectReason::InvalidUtf8);
    }
    REQUIRE(scheduler.find(source.key)->retire_reason == CaptionRetireReason::SourceLimit);
    REQUIRE(scheduler.complete(success(job), 2).reject == SchedulerRejectReason::RetiredSegment);
}

TEST_CASE("translation payload ceiling and strict UTF8 errors consume terminal result once")
{
    auto scheduler = make_scheduler();
    scheduler.register_segment(make_segment(1), {}, 0);
    auto job = scheduler.dispatch(0).job;
    std::string text;
    SchedulerRejectReason expected = SchedulerRejectReason::InvalidUtf8;
    SECTION("overlong") { text = "\xc0\xaf"; }
    SECTION("surrogate") { text = "\xed\xa0\x80"; }
    SECTION("outside Unicode") { text = "\xf4\x90\x80\x80"; }
    SECTION("truncated codepoint") { text = "\xe3\x81"; }
    SECTION("continuation") { text = "\x80"; }
    SECTION("empty") { expected = SchedulerRejectReason::EmptyText; }
    SECTION("over ceiling") {
        text = std::string(16385, 'x');
        expected = SchedulerRejectReason::TranslationLimit;
    }
    const auto result = scheduler.complete(success(job, text), 1);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.reject == expected);
    REQUIRE(result.outcome == TranslationOutcome::Failed);
    REQUIRE(result.effects.retired.size() == 1);
    REQUIRE(scheduler.in_flight_count() == 0);
    REQUIRE(scheduler.find(job->id.segment)->translated_text.empty());
    REQUIRE(scheduler.complete(success(job), 2).effects.retired.empty());
}

TEST_CASE("translation accepts exact UTF8 byte ceiling and cancellation confirms slot return")
{
    auto scheduler = make_scheduler();
    scheduler.register_segment(make_segment(1), {}, 0);
    auto job = scheduler.dispatch(0).job;
    const std::string boundary = std::string(16381, 'a') + "\xe3\x81\x82";
    REQUIRE(scheduler.complete(success(job, boundary), 1).accepted);
    REQUIRE(scheduler.find(job->id.segment)->translated_text == boundary);
    scheduler.register_segment(make_segment(2), {}, 1);
    auto cancelled = scheduler.dispatch(1).job;
    REQUIRE(scheduler.retire(cancelled->id.segment, CaptionRetireReason::Completed).accepted);
    REQUIRE(scheduler.in_flight_count() == 1);
    auto confirmation = failed(cancelled);
    confirmation.outcome = TranslationOutcome::Cancelled;
    REQUIRE(scheduler.complete(confirmation, 2).reject == SchedulerRejectReason::RetiredSegment);
    REQUIRE(scheduler.in_flight_count() == 0);
}

TEST_CASE("zero identifiers and exhausted monotonic identifiers cannot be reused")
{
    auto scheduler = make_scheduler();
    auto invalid = make_segment(0);
    REQUIRE(scheduler.register_segment(invalid, {}, 0).reject == SchedulerRejectReason::InvalidId);
    invalid = make_segment(1, 0);
    REQUIRE(scheduler.register_segment(invalid, {}, 0).reject == SchedulerRejectReason::InvalidId);
    auto maximum = make_segment(std::numeric_limits<uint64_t>::max());
    REQUIRE(scheduler.register_segment(maximum, {}, 0).accepted);
    REQUIRE(scheduler.retire(maximum.key, CaptionRetireReason::Completed).accepted);
    REQUIRE(scheduler.forget_retired(maximum.key));
    REQUIRE(scheduler.register_segment(maximum, {}, 0).reject == SchedulerRejectReason::ReusedId);
    REQUIRE(scheduler.register_segment(make_segment(1), {}, 0).reject == SchedulerRejectReason::ReusedId);
}
