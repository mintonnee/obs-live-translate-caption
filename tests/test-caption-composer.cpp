#include <catch2/catch_test_macros.hpp>
#include "caption-composer.hpp"
#include "caption-wrap.hpp"
#include <algorithm>
#include <limits>

using namespace lt;

namespace {

CaptionSegment segment(uint64_t id, uint64_t revision = 1, uint64_t generation = 1)
{
    return {{generation, 1, id}, {1, id}, revision, "source " + std::to_string(revision),
            false, {}, 0};
}

TranslationCompletion translated(const CaptionSegment &s, std::string text, uint64_t attempt = 1)
{
    return {{s.key, s.source_revision, attempt}, TranslationOutcome::Success,
             TranslationFailure::None, std::move(text), 0, 0};
}

CaptionComposer composer(CaptionComposerConfig cfg = {})
{
    CaptionComposer value(cfg);
    REQUIRE(value.reset_generation(1, 0).accepted);
    return value;
}

void ready(CaptionComposer &value, const CaptionSegment &s, const std::string &text, uint64_t now = 0)
{
    REQUIRE(value.register_segment(s, now).accepted);
    REQUIRE(value.complete(translated(s, text), now).accepted);
}

size_t events_of(const std::vector<CaptionDisplayEvent> &events, CaptionDisplayEventKind kind)
{
    return static_cast<size_t>(std::count_if(events.begin(), events.end(), [&](const auto &event) {
        return event.kind == kind;
    }));
}

void same_times(const CaptionDisplayStatus &before, const CaptionDisplayStatus &after)
{
    REQUIRE(before.first_visible_ms == after.first_visible_ms);
    REQUIRE(before.page_first_visible_ms == after.page_first_visible_ms);
    REQUIRE(before.page_min_until_ms == after.page_min_until_ms);
    REQUIRE(before.page_expires_ms == after.page_expires_ms);
}

void restarted_times(const CaptionDisplayStatus &status, uint64_t now, uint64_t hold = 4000)
{
    REQUIRE(status.first_visible_ms == now);
    REQUIRE(status.page_first_visible_ms == now);
    REQUIRE(status.page_min_until_ms == now + std::min<uint64_t>(hold, 1200));
    REQUIRE(status.page_expires_ms == now + hold);
}

const char kFourLines[] = "the quick brown fox jumps over the lazy dog and then naps in the sun";
const char kHangul30[] = "가나다라마바사아자차가나다라마바사아자차가나다라마바사아자차";

}

TEST_CASE("caption pages consume C A B responses into an ordered rolling window")
{
    auto value = composer();
    const auto a = segment(1), b = segment(2), c = segment(3);
    REQUIRE(value.register_segments({a, b, c}, 0).accepted);
    value.complete(translated(c, "C"), 100);
    REQUIRE_FALSE(value.tick(100).changed);
    value.complete(translated(a, "A"), 200);
    auto first = value.tick(200);
    REQUIRE(first.changed == "A");
    REQUIRE(first.snapshot.key == a.key);
    REQUIRE(events_of(first.events, CaptionDisplayEventKind::PageEntered) == 1);
    value.complete(translated(b, "B"), 300);
    REQUIRE_FALSE(value.tick(1399).changed);
    auto second = value.tick(1400);
    REQUIRE(second.changed == "A B");
    REQUIRE(second.snapshot.key == b.key);
    REQUIRE(events_of(second.events, CaptionDisplayEventKind::PageEntered) == 1);
    REQUIRE_FALSE(value.tick(1400).changed);
    REQUIRE_FALSE(value.tick(2599).changed);
    REQUIRE(value.tick(2600).changed == "A B C");
    REQUIRE(value.display_status(a.key)->retire_reason == CaptionRetireReason::Completed);
    REQUIRE(value.display_status(b.key)->retire_reason == CaptionRetireReason::Completed);
    REQUIRE(value.render(6600) == "");
}

TEST_CASE("caption segments share a line across utterances until the width limit")
{
    auto value = composer({2, 12, 4000});
    auto a = segment(1), b = segment(2), c = segment(3), d = segment(4);
    SECTION("final segments") { a.is_final = b.is_final = c.is_final = d.is_final = true; }
    SECTION("provisional segments") {}
    SECTION("mixed segments") { a.is_final = c.is_final = true; }
    d.key.utterance_id = 2;
    d.order_key = {2, 0};
    ready(value, a, "hello");
    ready(value, b, "world");
    ready(value, c, "again");
    ready(value, d, "fresh");

    REQUIRE(value.render(0) == "hello");
    REQUIRE(value.render(1200) == "hello world");
    REQUIRE(value.render(2400) == "hello world\nagain");
    REQUIRE(value.render(3600) == "hello world\nagain fresh");
}

TEST_CASE("caption joining segments preserves explicit newlines and in-place revisions")
{
    auto value = composer({3, 60, 4000});
    auto a = segment(1), b = segment(2);
    a.source_text = "first\nsecond";
    ready(value, a, "first\r\nsecond");
    ready(value, b, "third");
    REQUIRE(value.render(0) == "first\nsecond");
    REQUIRE(value.render(1200) == "first\nsecond third");

    b.source_revision = 2;
    b.source_text = "third\nfourth";
    REQUIRE(value.register_segment(b, 1300).accepted);
    REQUIRE(value.complete(translated(b, "third\nfourth"), 1400).accepted);
    REQUIRE(value.render(1400) == "first\nsecond third\nfourth");
}

TEST_CASE("caption joined segments still roll at the configured line limit")
{
    auto value = composer({1, 10, 4000});
    ready(value, segment(1), "hello");
    ready(value, segment(2), "world");
    ready(value, segment(3), "again");
    REQUIRE(value.render(0) == "hello");
    REQUIRE(value.render(1200) == "world");
    REQUIRE(value.render(2400) == "again");
}

TEST_CASE("caption pages preserve all four wrapped lines in a two line box")
{
    auto value = composer({2, 20, 4000});
    const auto lines = wrap_text(kFourLines, 20);
    REQUIRE(lines.size() == 4);
    ready(value, segment(1), kFourLines);
    REQUIRE(value.render(0) == lines[0] + "\n" + lines[1]);
    REQUIRE_FALSE(value.render(1199));
    REQUIRE(value.render(1200) == lines[2] + "\n" + lines[3]);
    REQUIRE(value.snapshot().display.page_index == 1);
    REQUIRE_FALSE(value.render(5199));
    REQUIRE(value.render(5200) == "");
    REQUIRE_FALSE(value.render(5201));
    REQUIRE(value.take_truncations().empty());
}

TEST_CASE("caption pages preserve CJK codepoints mixed widths and closing punctuation")
{
    auto value = composer({1, 40, 4000});
    auto lines = wrap_text(kHangul30, 40);
    REQUIRE(lines.size() == 2);
    ready(value, segment(1), kHangul30);
    REQUIRE(value.render(0) == lines[0]);
    REQUIRE(value.render(1200) == lines[1]);
    REQUIRE(lines[0] + lines[1] == kHangul30);
    value.set_config({1, 10, 4000}, 1300);
    REQUIRE(value.render(1300) == "가나다라마");
    REQUIRE(value.render(2400) == "바사아자차");

    auto punct = composer({1, 10, 4000});
    ready(punct, segment(1), "abcdefghij!next");
    REQUIRE(punct.render(0) == "abcdefghij!");
    REQUIRE(display_width(punct.snapshot().text) == 11);
    REQUIRE(punct.render(1200) == "next");
}

TEST_CASE("caption ordering uses order keys even with reverse identifier allocation")
{
    auto value = composer();
    auto late = segment(1), early = segment(2);
    late.order_key = {2, 0};
    early.order_key = {1, 0};
    ready(value, late, "later");
    ready(value, early, "earlier");
    REQUIRE(value.render(0) == "earlier");
    REQUIRE(value.render(1200) == "earlier later");
}

TEST_CASE("caption failures skip only failed slots and never compress ready pages into one tick")
{
    auto value = composer();
    for (uint64_t i = 1; i <= 4; ++i) value.register_segment(segment(i), 0);
    auto failure = translated(segment(1), "ignored");
    failure.outcome = TranslationOutcome::Failed;
    failure.failure = TranslationFailure::Network;
    auto failed = value.complete(failure, 10);
    REQUIRE(failed.events.size() == 1);
    REQUIRE(failed.events[0].reason == CaptionRetireReason::TranslationFailed);
    value.complete(translated(segment(2), "B"), 20);
    value.retire(segment(3).key, CaptionRetireReason::QueueLimit, 20);
    value.complete(translated(segment(4), "D"), 20);
    REQUIRE(value.render(20) == "B");
    REQUIRE_FALSE(value.render(1219));
    REQUIRE(value.render(1220) == "B D");
    REQUIRE_FALSE(value.complete(translated(segment(1), "resurrection"), 1300).accepted);
}

TEST_CASE("caption waits for unresolved earlier segments until explicit or stale retirement")
{
    auto value = composer();
    value.register_segment(segment(1), 0);
    ready(value, segment(2), "B", 1000);
    REQUIRE_FALSE(value.render(5999));
    const auto rendered = value.tick(6000);
    REQUIRE(rendered.changed == "B");
    REQUIRE(value.display_status(segment(1).key)->retire_reason ==
            CaptionRetireReason::StaleBeforeDisplay);
}

TEST_CASE("caption first page revisions replace in place and restart hold timers")
{
    auto value = composer({2, 20, 4000});
    auto a = segment(1);
    ready(value, a, "old", 100);
    ready(value, segment(2), "B", 100);
    REQUIRE(value.render(100) == "old");
    const auto original = value.snapshot().display;
    a = segment(1, 2);
    REQUIRE(value.register_segment(a, 200).accepted);
    REQUIRE_FALSE(value.render(200)); // Old text remains during translation.
    const auto result = value.complete(translated(a, kFourLines), 300);
    REQUIRE(events_of(result.events, CaptionDisplayEventKind::PageReplaced) == 1);
    REQUIRE(value.render(300) == "the quick brown fox\njumps over the lazy");
    REQUIRE(value.snapshot().key == a.key);
    REQUIRE(original.page_expires_ms == 4100);
    restarted_times(value.snapshot().display, 300);
    REQUIRE_FALSE(value.render(1499));
    REQUIRE(value.render(1500) == "dog and then naps in\nthe sun");
    REQUIRE(value.render(2700) == "dog and then naps in\nthe sun B");
}

TEST_CASE("caption visible revision replaces its rolling window line without duplicate append")
{
    auto value = composer();
    const auto a = segment(1);
    auto b = segment(2);
    ready(value, a, "A");
    ready(value, b, "B");
    REQUIRE(value.render(0) == "A");
    REQUIRE(value.render(1200) == "A B");
    const auto original = value.snapshot().display;

    b = segment(2, 2);
    REQUIRE(value.register_segment(b, 1300).accepted);
    REQUIRE(value.complete(translated(b, "B2"), 1400).accepted);
    REQUIRE(value.render(1400) == "A B2");
    REQUIRE(original.page_expires_ms == 5200);
    restarted_times(value.snapshot().display, 1400);
}

TEST_CASE("caption final confirmation refreshes hold once and preserves the reading interval")
{
    auto value = composer();
    auto a = segment(1);
    ready(value, a, "confirmed", 100);
    value.render(100);
    const auto original = value.snapshot().display;

    a.is_final = true;
    REQUIRE(value.register_segment(a, 4000).accepted);
    REQUIRE(value.render(4000) == "confirmed");
    const auto confirmed = value.snapshot().display;
    REQUIRE(confirmed.page_expires_ms == 8000);
    REQUIRE(confirmed.first_visible_ms == original.first_visible_ms);
    REQUIRE(confirmed.page_first_visible_ms == original.page_first_visible_ms);
    REQUIRE(confirmed.page_min_until_ms == original.page_min_until_ms);
    REQUIRE(value.register_segment(a, 5000).accepted);
    REQUIRE_FALSE(value.render(5000));
    same_times(confirmed, value.snapshot().display);

    value.set_config({2, 60, 6000}, 5100);
    value.render(5100);
    REQUIRE(value.snapshot().display.page_expires_ms == 10000);
    REQUIRE_FALSE(value.render(9999));
    REQUIRE(value.render(10000) == "");
}

TEST_CASE("caption final confirmation does not delay the next page")
{
    auto value = composer({1, 20, 4000});
    auto a = segment(1);
    ready(value, a, "first\nsecond\nthird");
    value.render(0);
    a.is_final = true;
    REQUIRE(value.register_segment(a, 1100).accepted);
    value.render(1100);
    REQUIRE(value.snapshot().display.page_expires_ms == 5100);
    REQUIRE(value.render(1200) == "second");
    REQUIRE(value.snapshot().display.page_expires_ms == 5200);
}

TEST_CASE("caption final revision preserves old display but rejects obsolete translation results")
{
    auto value = composer();
    auto a = segment(1);
    ready(value, a, "old", 100);
    value.render(100);
    a = segment(1, 2);
    REQUIRE(value.register_segment(a, 200).accepted);
    a.is_final = true;
    REQUIRE(value.register_segment(a, 4000).accepted);
    REQUIRE(value.render(4000) == "old");
    REQUIRE(value.snapshot().display.page_expires_ms == 8000);
    REQUIRE(value.complete(translated(segment(1), "stale"), 4100).reject ==
            CaptionComposeReject::StaleRevision);
    REQUIRE_FALSE(value.render(7999));
    REQUIRE(value.render(8000) == "");
    REQUIRE_FALSE(value.complete(translated(a, "late final"), 8001).accepted);
}

TEST_CASE("caption final notification targets only its visible snapshot and refreshes once")
{
    auto value = composer();
    const auto a = segment(1);
    ready(value, a, "visible", 100);
    value.render(100);
    REQUIRE(value.confirm_final(segment(1, 1, 2).key, 200).reject ==
            CaptionComposeReject::StaleGeneration);
    REQUIRE(value.confirm_final(segment(2).key, 200).reject == CaptionComposeReject::MissingSegment);
    REQUIRE_FALSE(value.render(200));
    REQUIRE(value.confirm_final(a.key, 4000).accepted);
    REQUIRE(value.render(4000) == "visible");
    REQUIRE(value.snapshot().display.page_expires_ms == 8000);
    REQUIRE(value.confirm_final(a.key, 5000).reject == CaptionComposeReject::Duplicate);
    REQUIRE_FALSE(value.render(7999));
    REQUIRE(value.confirm_final(a.key, 8000).reject == CaptionComposeReject::MissingSegment);
    REQUIRE(value.render(8000) == "");
}

TEST_CASE("caption expiry wins final confirmation in either call order")
{
    auto value = composer();
    auto a = segment(1);
    ready(value, a, "old", 100);
    value.render(100);
    a.is_final = true;
    SECTION("confirmation before tick") {
        REQUIRE(value.register_segment(a, 4100).reject == CaptionComposeReject::RetiredSegment);
        REQUIRE(value.render(4100) == "");
    }
    SECTION("tick before confirmation") {
        REQUIRE(value.render(4100) == "");
        REQUIRE(value.register_segment(a, 4100).reject == CaptionComposeReject::RetiredSegment);
    }
    REQUIRE_FALSE(value.render(4101));
}

TEST_CASE("caption retries retain visible result and reject old revision attempt and duplicates")
{
    auto value = composer();
    ready(value, segment(1), "v1");
    value.render(0);
    const auto original = value.snapshot().display;
    value.register_segment(segment(1, 2), 100);
    REQUIRE(value.complete(translated(segment(1, 2), "v2"), 101).accepted);
    REQUIRE(value.render(101) == "v2");
    REQUIRE(value.complete(translated(segment(1), "old"), 102).reject ==
            CaptionComposeReject::StaleRevision);
    REQUIRE(value.expect_job({segment(1).key, 2, 2}, 103).accepted);
    REQUIRE_FALSE(value.render(103));
    REQUIRE(value.complete(translated(segment(1, 2), "a2", 2), 104).accepted);
    REQUIRE(value.render(104) == "a2");
    REQUIRE(value.complete(translated(segment(1, 2), "a1"), 105).reject ==
            CaptionComposeReject::StaleAttempt);
    auto failure = translated(segment(1, 2), "", 2);
    failure.outcome = TranslationOutcome::Failed;
    REQUIRE(value.complete(failure, 106).reject == CaptionComposeReject::Duplicate);
    REQUIRE(original.page_expires_ms == 4000);
    restarted_times(value.snapshot().display, 104);
    value.register_segment(segment(1, 3), 107);
    failure.id = {segment(1).key, 3, 1};
    REQUIRE(value.complete(failure, 108).accepted);
    REQUIRE_FALSE(value.render(108));
    REQUIRE(value.snapshot().text == "a2");
}

TEST_CASE("caption next page retains accepted job identity while a new revision is pending")
{
    auto value = composer({1, 20, 4000});
    ready(value, segment(1), "first\nsecond\nthird");
    value.render(0);
    value.register_segment(segment(1, 2), 100);
    REQUIRE(value.render(1200) == "second");
    REQUIRE(value.snapshot().job.source_revision == 1);
}

TEST_CASE("caption middle page correction excludes completed prefix without replay")
{
    auto value = composer({1, 20, 4000});
    ready(value, segment(1), "first\nsecond\nthird\nfourth");
    REQUIRE(value.render(0) == "first");
    REQUIRE(value.render(1200) == "second");
    const auto original = value.snapshot().display;
    value.register_segment(segment(1, 2), 1300);
    REQUIRE(value.complete(translated(segment(1, 2), "first\nCHANGED\nthird\nfourth"), 1300).accepted);
    REQUIRE(value.render(1300) == "CHANGED");
    REQUIRE(value.snapshot().display.page_index == 1);
    REQUIRE(original.page_expires_ms == 5200);
    restarted_times(value.snapshot().display, 1300);
    REQUIRE(value.render(2500) == "third");
    REQUIRE(value.render(3700) == "fourth");
}

TEST_CASE("caption ambiguous middle correction replaces current ordinal and drops remainder")
{
    auto value = composer({1, 20, 4000});
    ready(value, segment(1), "first\nsecond\nthird");
    value.render(0);
    value.render(1200);
    const auto original = value.snapshot().display;
    value.register_segment(segment(1, 2), 1300);
    auto result = value.complete(translated(segment(1, 2), "changed first\nchanged second\nnew tail"),
                                 1300);
    REQUIRE(result.accepted);
    REQUIRE(events_of(result.events, CaptionDisplayEventKind::RemainderDiscarded) == 1);
    REQUIRE(result.events[0].reason == CaptionRetireReason::ReconcileAmbiguous);
    REQUIRE(value.render(1300) == "changed second");
    REQUIRE(original.page_expires_ms == 5200);
    restarted_times(value.snapshot().display, 1300);
    REQUIRE(value.snapshot().display.page_index == 1);
    REQUIRE_FALSE(value.render(2400));
    REQUIRE_FALSE(value.render(5299));
    REQUIRE(value.render(5300) == "");
}

TEST_CASE("caption ambiguous shortening clamps to last new page without index rewind")
{
    auto value = composer({1, 20, 4000});
    ready(value, segment(1), "first\nsecond\nthird");
    value.render(0);
    value.render(1200);
    value.render(2400);
    value.register_segment(segment(1, 2), 2500);
    value.complete(translated(segment(1, 2), "short replacement"), 2500);
    REQUIRE(value.render(2500) == "short replacement");
    REQUIRE(value.snapshot().display.page_index == 2);
    REQUIRE_FALSE(value.render(3600));
}

TEST_CASE("caption exact page expiry dominates a correction in either call order")
{
    auto value = composer();
    ready(value, segment(1), "old");
    value.render(0);
    value.register_segment(segment(1, 2), 3900);
    SECTION("completion before tick") {
        REQUIRE_FALSE(value.complete(translated(segment(1, 2), "late"), 4000).accepted);
        REQUIRE(value.render(4000) == "");
    }
    SECTION("tick before completion") {
        REQUIRE(value.render(4000) == "");
        REQUIRE_FALSE(value.complete(translated(segment(1, 2), "late"), 4000).accepted);
    }
    REQUIRE_FALSE(value.render(4001));
    REQUIRE(value.display_status(segment(1).key)->state == CaptionDisplayState::Retired);
}

TEST_CASE("caption expired correction cannot overwrite a prepared next page")
{
    auto value = composer({1, 20, 4000});
    ready(value, segment(1), "first\nsecond");
    value.render(0);
    value.register_segment(segment(1, 2), 3900);
    REQUIRE(value.complete(translated(segment(1, 2), "changed"), 4000).reject ==
            CaptionComposeReject::Expired);
    REQUIRE(value.render(4000) == "second");
    REQUIRE(value.snapshot().display.page_index == 1);
    REQUIRE(value.complete(translated(segment(1, 2), "changed"), 4001).reject ==
            CaptionComposeReject::Duplicate);
    REQUIRE_FALSE(value.render(4001));
}

TEST_CASE("caption one to many replacement waits for its head and transfers slot once")
{
    auto value = composer();
    ready(value, segment(1), "old", 100);
    value.render(100);
    const auto original = value.snapshot().display;
    auto a = segment(2), b = segment(3);
    a.order_key = {1, 10};
    b.order_key = {1, 11};
    a.replaces = b.replaces = {segment(1).key};
    auto registration = value.register_segments({b, a}, 200);
    REQUIRE(registration.accepted);
    REQUIRE(value.display_status(segment(1).key)->retire_reason == CaptionRetireReason::Replaced);
    REQUIRE_FALSE(value.complete(translated(segment(1), "old callback"), 210).accepted);
    value.complete(translated(b, "B"), 300);
    REQUIRE_FALSE(value.render(300));
    REQUIRE(value.snapshot().text == "old");
    value.complete(translated(a, "A"), 400);
    const auto frame = value.tick(400);
    REQUIRE(frame.changed == "A");
    REQUIRE(events_of(frame.events, CaptionDisplayEventKind::PageReplaced) == 1);
    REQUIRE(original.page_expires_ms == 4100);
    restarted_times(frame.snapshot.display, 400);
    REQUIRE_FALSE(value.render(1599));
    REQUIRE(value.render(1600) == "A B");
    REQUIRE(value.snapshot().display.first_visible_ms == 1600);
    REQUIRE(value.snapshot().display.page_expires_ms == 5600);
}

TEST_CASE("caption many to one replacement inherits the earliest live slot")
{
    auto value = composer();
    ready(value, segment(1), "A", 100);
    ready(value, segment(2), "B", 100);
    ready(value, segment(3), "C", 100);
    value.render(100);
    const auto original = value.snapshot().display;
    auto merged = segment(4);
    merged.order_key = {9, 0};
    merged.replaces = {segment(2).key, segment(1).key};
    REQUIRE(value.register_segment(merged, 200).accepted);
    value.complete(translated(merged, "merged"), 300);
    REQUIRE(value.render(300) == "merged");
    REQUIRE(original.page_expires_ms == 4100);
    restarted_times(value.snapshot().display, 300);
    REQUIRE(value.render(1500) == "merged C");
    REQUIRE(value.display_status(segment(2).key)->retire_reason == CaptionRetireReason::Replaced);
}

TEST_CASE("caption replacement missing inherited expiry never resurrects an old slot")
{
    auto value = composer();
    ready(value, segment(1), "old");
    value.render(0);
    auto replacement = segment(2);
    replacement.replaces = {segment(1).key};
    value.register_segment(replacement, 100);
    REQUIRE(value.render(4000) == "");
    REQUIRE_FALSE(value.complete(translated(replacement, "too late"), 4000).accepted);
    REQUIRE_FALSE(value.render(4001));
}

TEST_CASE("caption repeated replacement chains restart hold from the latest visible result")
{
    auto value = composer();
    ready(value, segment(1), "old");
    value.render(0);
    for (uint64_t id = 2; id <= 6; ++id) {
        auto replacement = segment(id);
        replacement.replaces = {segment(id - 1).key};
        value.register_segment(replacement, id * 100);
        value.complete(translated(replacement, "replacement " + std::to_string(id)), id * 100);
        REQUIRE(value.render(id * 100));
        restarted_times(value.snapshot().display, id * 100);
    }
    REQUIRE_FALSE(value.render(4599));
    REQUIRE(value.render(4600) == "");
}

TEST_CASE("caption replacement touching completed history is conservatively retired")
{
    auto value = composer();
    ready(value, segment(1), "A");
    ready(value, segment(2), "B");
    value.render(0);
    value.render(1200);
    auto merged = segment(3);
    merged.replaces = {segment(1).key, segment(2).key};
    const auto result = value.register_segment(merged, 1300);
    REQUIRE(result.reject == CaptionComposeReject::ReconcileAmbiguous);
    REQUIRE(value.display_status(merged.key)->retire_reason == CaptionRetireReason::ReconcileAmbiguous);
    REQUIRE_FALSE(value.complete(translated(merged, "replay"), 1301).accepted);
}

TEST_CASE("caption whole segment lifetime protects current minimum then drops the tail")
{
    auto value = composer({1, 20, 30000});
    std::string text;
    for (int i = 0; i < 20; ++i) text += "page " + std::to_string(i) + "\n";
    ready(value, segment(1), text);
    REQUIRE(value.render(0) == "page 0");
    for (uint64_t now = 1200; now <= 14400; now += 1200) REQUIRE(value.render(now));
    REQUIRE(value.snapshot().display.page_index == 12);
    REQUIRE_FALSE(value.render(15000));
    REQUIRE_FALSE(value.render(15599));
    const auto expired = value.tick(15600);
    REQUIRE(expired.changed == "");
    REQUIRE(value.display_status(segment(1).key)->retire_reason == CaptionRetireReason::DisplayLifetime);
    REQUIRE_FALSE(value.complete(translated(segment(1), "late"), 16000).accepted);
}

TEST_CASE("caption single page respects a hold longer than the page lifetime cap")
{
    auto value = composer({2, 60, 30000});
    ready(value, segment(1), "single");
    value.render(0);
    REQUIRE_FALSE(value.render(29999));
    REQUIRE(value.render(30000) == "");
    REQUIRE(value.display_status(segment(1).key)->retire_reason == CaptionRetireReason::Completed);
}

TEST_CASE("caption box rewrap includes current and remaining but never completed prefix")
{
    auto value = composer({1, 20, 4000});
    ready(value, segment(1), "first\nabcdefghij klmnopqrst\nlast");
    value.render(0);
    REQUIRE(value.render(1200) == "abcdefghij");
    const auto original = value.snapshot().display;
    value.set_config({2, 10, 4000}, 1300);
    REQUIRE(value.render(1300) == "abcdefghij\nklmnopqrst");
    same_times(original, value.snapshot().display);
    REQUIRE(value.snapshot().display.page_index == 1);
    REQUIRE(value.render(2400) == "klmnopqrst\nlast");
    REQUIRE(value.snapshot().text.find("first") == std::string::npos);
}

TEST_CASE("caption changing width on first page rewraps immediately without losing remainder")
{
    auto value = composer({2, 120, 4000});
    ready(value, segment(1), kFourLines);
    REQUIRE(value.render(0) == kFourLines);
    value.set_config({2, 20, 4000});
    REQUIRE(value.render(100) == "the quick brown fox\njumps over the lazy");
    REQUIRE(value.render(1200) == "dog and then naps in\nthe sun");
}

TEST_CASE("caption hold change computes deadline from original current page time")
{
    auto value = composer();
    ready(value, segment(1), "held", 100);
    value.render(100);
    value.set_config({2, 60, 2000}, 1000);
    REQUIRE(value.tick(1000).snapshot.display.page_expires_ms == 2100);
    REQUIRE_FALSE(value.render(2099));
    REQUIRE(value.render(2100) == "");
    value.set_config({2, 60, 30000}, 2101);
    REQUIRE_FALSE(value.render(2101));
}

TEST_CASE("caption overload retirement preserves visible minimum but disables callbacks at once")
{
    auto value = composer();
    ready(value, segment(1), "visible");
    value.render(0);
    REQUIRE(value.retire(segment(1).key, CaptionRetireReason::QueueLimit, 100).accepted);
    REQUIRE_FALSE(value.render(100));
    REQUIRE_FALSE(value.complete(translated(segment(1), "resurrection"), 101).accepted);
    REQUIRE_FALSE(value.render(1199));
    REQUIRE(value.render(1200) == "");
    REQUIRE(value.display_status(segment(1).key)->retire_reason == CaptionRetireReason::QueueLimit);
}

TEST_CASE("caption one thousand inputs keep waiting and managed metadata bounded")
{
    auto value = composer();
    ready(value, segment(1), "visible");
    value.render(0);
    for (uint64_t id = 2; id <= 1001; ++id) {
        value.register_segment(segment(id), 100);
        REQUIRE(value.managed_count() <= 256);
        REQUIRE(value.waiting_count() <= 24);
    }
    REQUIRE(value.snapshot().text == "visible");
    REQUIRE(value.display_status(segment(2).key)->retire_reason == CaptionRetireReason::SegmentLimit);
    REQUIRE(value.forget_retired(segment(2).key));
    REQUIRE(value.register_segment(segment(2), 100).reject == CaptionComposeReject::ReusedId);
    REQUIRE(value.register_segment(segment(1002), 100).accepted);
}

TEST_CASE("caption snapshots distinguish equal text across segments and reset generations")
{
    auto value = composer();
    ready(value, segment(1), "same");
    ready(value, segment(2), "same");
    const auto first = value.tick(0).snapshot;
    const auto second = value.tick(1200);
    REQUIRE(second.changed == "same same");
    REQUIRE(second.snapshot.publication > first.publication);
    REQUIRE(second.snapshot.key == segment(2).key);
    const auto reset = value.reset_generation(2, 1300);
    REQUIRE(reset.accepted);
    REQUIRE(value.render(1300) == "");
    REQUIRE_FALSE(value.complete(translated(segment(2), "old generation"), 1300).accepted);
    REQUIRE_FALSE(value.reset_generation(1, 1300).accepted);
    ready(value, segment(3, 1, 2), "new", 1400);
    REQUIRE(value.render(1400) == "new");
    REQUIRE(value.snapshot().publication > second.snapshot.publication);
}

TEST_CASE("caption config clamps original ranges and minimum follows a short hold")
{
    auto value = composer({0, 4, 10});
    REQUIRE(value.config().max_lines == 1);
    REQUIRE(value.config().max_width == 10);
    REQUIRE(value.config().hold_ms == 1000);
    ready(value, segment(1), "A");
    ready(value, segment(2), "B");
    value.render(0);
    REQUIRE_FALSE(value.render(999));
    REQUIRE(value.render(1000) == "B");
    value.set_config({9, 500, 600000});
    REQUIRE(value.config().max_lines == 6);
    REQUIRE(value.config().max_width == 120);
    REQUIRE(value.config().hold_ms == 30000);
}

TEST_CASE("caption whitespace or oversized translations retire only without an older result")
{
    auto value = composer();
    value.register_segment(segment(1), 0);
    REQUIRE(value.complete(translated(segment(1), "   \n  "), 1).reject ==
            CaptionComposeReject::EmptyTranslation);
    REQUIRE(value.display_status(segment(1).key)->retire_reason == CaptionRetireReason::TranslationFailed);
    ready(value, segment(2), "keep", 2);
    value.render(2);
    value.register_segment(segment(2, 2), 3);
    REQUIRE(value.complete(translated(segment(2, 2), std::string(16385, 'a')), 3).reject ==
            CaptionComposeReject::ResourceLimit);
    REQUIRE_FALSE(value.render(3));
    REQUIRE(value.snapshot().text == "keep");
}

TEST_CASE("caption compatibility adapter preserves context failure clear and default settings")
{
    CaptionComposer value;
    REQUIRE(value.config().max_lines == 2);
    REQUIRE(value.config().max_width == 60);
    REQUIRE(value.config().hold_ms == 4000);
    REQUIRE_FALSE(value.render(0));
    value.push_final(1, "one", 0);
    value.push_final(2, "two", 0);
    value.push_final(1, "duplicate", 0);
    REQUIRE(value.pending_count() == 2);
    REQUIRE(value.context(2) == std::vector<std::string>{"one", "two"});
    value.on_failed(1, "failure", 0);
    value.on_translated(2, "second", 0);
    REQUIRE(value.render(0) == "second");
    REQUIRE(value.pending_count() == 0);
    value.clear();
    REQUIRE(value.context(3).empty());
    REQUIRE(value.render(1) == "");
    value.on_translated(2, "late", 1);
    REQUIRE_FALSE(value.render(1));
    value.push_final(3, "three", 2);
    value.on_translated(3, "third", 2);
    REQUIRE(value.render(2) == "third");
    REQUIRE(value.take_truncations().empty());
}

TEST_CASE("caption compatibility source context remains capped at sixteen")
{
    CaptionComposer value;
    for (uint64_t id = 1; id <= 20; ++id)
        value.push_final(id, "source " + std::to_string(id), id);
    REQUIRE(value.context(100).size() == 16);
    REQUIRE(value.context(100).front() == "source 5");
    REQUIRE(value.context(100).back() == "source 20");
    REQUIRE(value.context(0).empty());
}

TEST_CASE("caption rewrap preserves explicit blank lines without replaying separators")
{
    auto value = composer({1, 20, 4000});
    ready(value, segment(1), "first\n\nsecond\nthird");
    REQUIRE(value.render(0) == "first");
    REQUIRE(value.tick(1200).snapshot.display.page_index == 1);
    value.set_config({2, 20, 4000}, 1300);
    REQUIRE(value.render(1300) == "\nsecond");
    REQUIRE(value.render(2400) == "second\nthird");
}

TEST_CASE("caption retired replacement ancestor cannot reinsert beyond rolling history")
{
    auto value = composer();
    ready(value, segment(1), "old");
    value.render(0);
    auto replacement = segment(2);
    replacement.replaces = {segment(1).key};
    ready(value, replacement, "replacement", 100);
    value.render(100);
    ready(value, segment(3), "next", 200);
    REQUIRE_FALSE(value.render(1299));
    REQUIRE(value.render(1300) == "replacement next");
    auto replay = segment(4);
    replay.replaces = {segment(1).key};
    REQUIRE(value.register_segment(replay, 1300).reject == CaptionComposeReject::ReconcileAmbiguous);
    REQUIRE_FALSE(value.complete(translated(replay, "replayed"), 1301).accepted);
    REQUIRE_FALSE(value.render(1301));
}

TEST_CASE("caption replacement of a middle page inherits prefix and restarts timestamps")
{
    auto value = composer({1, 20, 4000});
    auto original_segment = segment(1);
    SECTION("provisional pages") {}
    SECTION("final pages") { original_segment.is_final = true; }
    ready(value, original_segment, "first\nsecond\nthird");
    value.render(0);
    value.render(1200);
    const auto original = value.snapshot().display;
    auto replacement = segment(2);
    replacement.is_final = original_segment.is_final;
    replacement.replaces = {segment(1).key};
    ready(value, replacement, "first\ncorrected\nthird", 1300);
    REQUIRE(value.render(1300) == "corrected");
    REQUIRE(value.snapshot().display.page_index == 1);
    REQUIRE(original.page_expires_ms == 5200);
    restarted_times(value.snapshot().display, 1300);
    REQUIRE(value.render(2500) == "third");
}
