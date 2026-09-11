#include <catch2/catch_test_macros.hpp>
#include "caption-segmenter.hpp"
#include "caption-wrap.hpp"
#include <algorithm>
#include <string>

namespace {

std::string repeat(const std::string &value, size_t count)
{
    std::string text;
    for (size_t i = 0; i < count; ++i) text += value;
    return text;
}

std::string content(const std::vector<std::string> &parts)
{
    std::string out;
    for (const auto &part : parts)
        for (char c : part)
            if (c != ' ' && c != '\n') out += c;
    return out;
}

std::string current(const lt::CaptionSegmenter &segmenter)
{
    std::vector<std::string> text;
    for (const auto &segment : segmenter.segments()) text.push_back(segment.source_text);
    return content(text);
}

lt::SegmenterResult stable(lt::CaptionSegmenter &segmenter, const std::string &prefix)
{
    segmenter.interim(prefix + " a", 0);
    segmenter.interim(prefix + " b", 400);
    return segmenter.interim(prefix + " c", 800);
}

bool valid_utf8(const std::string &text)
{
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 :
                         c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!n || i + n > text.size()) return false;
        for (size_t j = 1; j < n; ++j)
            if ((static_cast<unsigned char>(text[i + j]) & 0xc0) != 0x80) return false;
        i += n;
    }
    return true;
}

}

TEST_CASE("segment final preserves mixed scripts and code point boundaries")
{
    for (const auto &text : {repeat("가나다라마", 90), repeat("あいうえお", 90),
                             repeat("word 한국어 かな é 😀 ", 90)}) {
        const auto split = lt::split_caption_source(text);
        REQUIRE(split.size() > 1);
        REQUIRE(content(split) == content({lt::normalize_caption_source(text)}));
        for (const auto &part : split) {
            REQUIRE(valid_utf8(part));
            REQUIRE(lt::display_width(part) <= 120);
        }
    }
}

TEST_CASE("segment boundary ranking preserves decimals and short final tails")
{
    const auto text = std::string("Value is 3.14 and grows.") + repeat("x", 130);
    const auto parts = lt::split_caption_source(text);
    REQUIRE(parts.front() == "Value is 3.14 and grows.");
    REQUIRE(parts.back() == repeat("x", 10));
    REQUIRE(lt::split_caption_source("Hi! tail") ==
            std::vector<std::string>{"Hi!", "tail"});
    REQUIRE(lt::split_caption_source(repeat("x", 50) + ", " + repeat("y", 90)).front() ==
            repeat("x", 50) + ",");
    REQUIRE(lt::split_caption_source(repeat("x", 50) + "\n" + repeat("y", 90)).front() ==
            repeat("x", 50));
}

TEST_CASE("segment normalization keeps semantic symbols")
{
    REQUIRE(lt::normalize_caption_source("  안\t하지  않는다.\r\n3.14　! ") ==
            "안 하지 않는다.\n3.14 !");
    REQUIRE(lt::split_caption_source(" \r\n　").empty());
}

TEST_CASE("segment interim needs three distinct snapshots and exactly 800 ms")
{
    lt::CaptionSegmenter s;
    REQUIRE(s.interim("A complete sentence. a", 0).upserts.empty());
    REQUIRE(s.interim("A complete sentence. b", 400).upserts.empty());
    REQUIRE(s.interim("A complete sentence. c", 799).upserts.empty());
    REQUIRE(s.snapshot_count() == 3);
    auto result = s.tick(800);
    REQUIRE(result.upserts.size() == 1);
    REQUIRE(result.upserts[0].source_text == "A complete sentence.");
    REQUIRE(s.tick(10000).upserts.empty());
    REQUIRE(s.interim("A complete sentence. c", 10001).upserts.empty());
}

TEST_CASE("segment duplicate snapshots and ticks never manufacture observations")
{
    lt::CaptionSegmenter s;
    s.interim("Stable sentence. unfinished", 0);
    for (uint64_t time = 1000; time < 10000; time += 1000) {
        REQUIRE(s.interim("Stable sentence. unfinished", time).upserts.empty());
        REQUIRE(s.tick(time).upserts.empty());
    }
    REQUIRE(s.snapshot_count() == 1);
}

TEST_CASE("segment timeout still requires stability and leaves unfinished word")
{
    lt::CaptionSegmenter s;
    s.interim("This stable clause remains unfinishedA", 0);
    s.interim("This stable clause remains unfinishedB", 400);
    REQUIRE(s.interim("This stable clause remains unfinishedC", 800).upserts.empty());
    REQUIRE(s.tick(1999).upserts.empty());
    const auto emitted = s.tick(2000);
    REQUIRE(emitted.upserts.size() == 1);
    REQUIRE(emitted.upserts[0].source_text == "This stable clause remains");
}

TEST_CASE("segment no space interim leaves four code points")
{
    lt::CaptionSegmenter s;
    const auto prefix = repeat("가", 44);
    s.interim(prefix + "나", 0);
    s.interim(prefix + "다", 400);
    auto emitted = s.interim(prefix + "라", 800);
    REQUIRE(emitted.upserts.size() == 1);
    REQUIRE(emitted.upserts[0].source_text == repeat("가", 40));
}

TEST_CASE("segment final confirms without revision and only appends new tail")
{
    lt::CaptionSegmenter s;
    const auto initial = stable(s, "The original sentence.");
    REQUIRE(initial.upserts.size() == 1);
    const auto final = s.final("The original sentence. A new tail.", 900);
    REQUIRE(final.retired.empty());
    REQUIRE(s.segments().size() == 2);
    REQUIRE(s.segments()[0].key == initial.upserts[0].key);
    REQUIRE(s.segments()[0].source_revision == 1);
    REQUIRE(s.segments()[0].is_final);
    REQUIRE(s.segments()[1].source_text == "A new tail.");
}

TEST_CASE("segment SMART word extension does not mistake a token prefix for unchanged source")
{
    lt::CaptionSegmenter s;
    s.interim("The stable clause unfinishedA", 0);
    s.interim("The stable clause unfinishedB", 400);
    s.interim("The stable clause unfinishedC", 800);
    REQUIRE(s.tick(2000).upserts.size() == 1);
    const auto key = s.segments()[0].key;
    const auto final = s.final("The stable clauses", 2100);
    REQUIRE(final.upserts.size() == 1);
    REQUIRE(final.upserts[0].key == key);
    REQUIRE(final.upserts[0].source_revision == 2);
    REQUIRE(final.upserts[0].source_text == "The stable clauses");
}

TEST_CASE("segment newly observed tail has its own observation and timeout")
{
    lt::CaptionSegmenter s;
    stable(s, "First sentence.");
    s.interim("First sentence. A newly observed clause unfinishedA", 3000);
    s.interim("First sentence. A newly observed clause unfinishedB", 3400);
    REQUIRE(s.interim("First sentence. A newly observed clause unfinishedC", 3800)
                .upserts.empty());
    REQUIRE(s.tick(4999).upserts.empty());
    const auto tail = s.tick(5000);
    REQUIRE(tail.upserts.size() == 1);
    REQUIRE(tail.upserts[0].source_text == "A newly observed clause");
    REQUIRE(tail.upserts[0].first_seen_ms == 3000);
}

TEST_CASE("segment SMART single edits preserve identity and increase revision")
{
    for (const auto &corrected : {"이번에는 금요일에 간다.", "금요일 간다.",
                                  "월요일에 간다.", "금요일에 가지 않는다.",
                                  "금요일에 3명이 간다."}) {
        lt::CaptionSegmenter s;
        const auto initial = stable(s, "금요일에 간다.");
        REQUIRE(initial.upserts.size() == 1);
        const auto updated = s.final(corrected, 900);
        REQUIRE(updated.upserts.size() == 1);
        REQUIRE(updated.upserts[0].source_text == corrected);
        REQUIRE(updated.upserts[0].key == initial.upserts[0].key);
        REQUIRE(updated.upserts[0].source_revision == 2);
    }
}

TEST_CASE("segment numeric and negation changes are never whitespace normalization")
{
    for (const auto &corrected : {"We pay 14 dollars.", "We do not pay 3.14 dollars."}) {
        lt::CaptionSegmenter s;
        stable(s, "We pay 3.14 dollars.");
        const auto out = s.final(corrected, 900);
        REQUIRE(out.upserts.size() == 1);
        REQUIRE(out.upserts.front().source_revision == 2);
        REQUIRE(current(s) == content({corrected}));
    }
}

TEST_CASE("segment SMART merges and splits record replacement relations")
{
    lt::CaptionSegmenter s;
    stable(s, "First sentence.");
    s.interim("First sentence. Second sentence. a", 1000);
    s.interim("First sentence. Second sentence. b", 1400);
    s.interim("First sentence. Second sentence. c", 1800);
    REQUIRE(s.segments().size() == 2);
    const auto before = s.segments();
    const auto merge = s.final("First sentence and second sentence.", 2000);
    REQUIRE(merge.upserts.size() == 1);
    REQUIRE(merge.upserts[0].replaces ==
            std::vector<lt::SegmentKey>{before[0].key, before[1].key});
    REQUIRE(merge.retired.size() == 2);

    lt::CaptionSegmenter split;
    stable(split, "An original sentence.");
    const auto out = split.final(repeat("a", 119) + ". " + repeat("b", 80) + ".", 900);
    REQUIRE(out.upserts.size() == 2);
    REQUIRE(out.upserts[0].replaces.size() == 1);
    REQUIRE(out.upserts[0].replaces == out.upserts[1].replaces);
    REQUIRE(out.upserts[0].order_key < out.upserts[1].order_key);
}

TEST_CASE("segment repeated words and complete rewrite preserve authoritative content")
{
    for (const auto &corrected : {"go go stop go.", "Entirely different final text!"}) {
        lt::CaptionSegmenter s;
        stable(s, "go go go go.");
        s.final(corrected, 900);
        REQUIRE(current(s) == content({corrected}));
    }
}

TEST_CASE("segment ambiguous correction across retired text never replays it")
{
    lt::CaptionSegmenter s;
    stable(s, "First sentence.");
    s.set_display_state(s.segments()[0].key, lt::CaptionDisplayState::Retired);
    const auto correction = s.final("Changed first sentence. Additional content.", 900);
    REQUIRE(correction.upserts.empty());
    REQUIRE(correction.reconcile_ambiguous == 1);
    REQUIRE(correction.retired[0].reason == lt::CaptionRetireReason::ReconcileAmbiguous);
}

TEST_CASE("segment proven new tail after retired prefix is allowed")
{
    lt::CaptionSegmenter s;
    stable(s, "First sentence.");
    s.set_display_state(s.segments()[0].key, lt::CaptionDisplayState::Retired);
    auto final = s.final("First sentence. New tail.", 900);
    REQUIRE(final.reconcile_ambiguous == 0);
    REQUIRE(s.segments().size() == 2);
    REQUIRE(s.segments()[1].source_text == "New tail.");
}

TEST_CASE("segment identical consecutive utterances and resets never reuse ids")
{
    lt::CaptionSegmenter s;
    const auto first = s.final("Same sentence.", 0).upserts.at(0).key;
    const auto second = s.final("Same sentence.", 10).upserts.at(0).key;
    REQUIRE(second.utterance_id > first.utterance_id);
    REQUIRE(second.segment_id > first.segment_id);
    s.reset(2);
    const auto third = s.final("Same sentence.", 20).upserts.at(0).key;
    REQUIRE(third.generation == 2);
    REQUIRE(third.utterance_id > second.utterance_id);
    REQUIRE(third.segment_id > second.segment_id);
    REQUIRE_THROWS_AS(s.reset(0), std::invalid_argument);
}

TEST_CASE("segment snapshot limits stop interim and bound UTF8 final tail")
{
    lt::CaptionSegmenter s;
    const auto long_text = repeat("가", 12000);
    auto interim = s.interim(long_text, 0);
    REQUIRE(interim.source_limit == 1);
    REQUIRE(interim.upserts.empty());
    REQUIRE(s.incremental_stopped());
    REQUIRE(s.snapshot_bytes() <= 3 * lt::CaptionSegmenter::kMaxSourceBytes);
    const auto final = s.final(long_text, 1000);
    REQUIRE(final.source_limit == 1);
    REQUIRE(s.managed_count() <= lt::CaptionSegmenter::kMaxSegments);
    REQUIRE(current(s).size() <= lt::CaptionSegmenter::kMaxSourceBytes);
    for (const auto &segment : s.segments()) REQUIRE(valid_utf8(segment.source_text));
}

TEST_CASE("segment synthetic thousand segment burst keeps at most 256")
{
    lt::CaptionSegmenter s;
    const auto result = s.final(repeat(repeat("x", 119) + ".", 1000), 0);
    REQUIRE(result.source_limit == 1);
    REQUIRE(result.segment_limit == 1);
    REQUIRE(s.managed_count() == lt::CaptionSegmenter::kMaxSegments);
    REQUIRE(result.upserts.size() == lt::CaptionSegmenter::kMaxSegments);
}

TEST_CASE("segment final only mode keeps splitting and final revisions")
{
    lt::CaptionSegmenter s;
    s.set_incremental(false);
    REQUIRE(stable(s, "A stable sentence.").upserts.empty());
    REQUIRE(s.tick(3000).upserts.empty());
    const auto final = s.final(repeat("가", 100), 4000);
    REQUIRE(final.upserts.size() == 2);
    REQUIRE(final.upserts[0].is_final);
}
