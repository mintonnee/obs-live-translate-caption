#include <catch2/catch_test_macros.hpp>
#include "caption-composer.hpp"
#include "caption-wrap.hpp"

#include <string>
#include <vector>

using namespace lt;

namespace {

CaptionComposerConfig make_config(int max_lines, uint64_t hold_ms)
{
    CaptionComposerConfig cfg;
    cfg.max_lines = max_lines;
    cfg.hold_ms = hold_ms;
    return cfg;
}

CaptionComposerConfig make_config(int max_lines, int max_width, uint64_t hold_ms)
{
    CaptionComposerConfig cfg;
    cfg.max_lines = max_lines;
    cfg.max_width = max_width;
    cfg.hold_ms = hold_ms;
    return cfg;
}

std::vector<std::string> split_lines(const std::string &text)
{
    std::vector<std::string> out;
    size_t begin = 0;
    for (;;) {
        const size_t nl = text.find('\n', begin);
        if (nl == std::string::npos) {
            out.push_back(text.substr(begin));
            return out;
        }
        out.push_back(text.substr(begin, nl - begin));
        begin = nl + 1;
    }
}

// A 102-character Latin sentence: one line at width 60 is impossible, two fit.
const char kTwoLineText[] =
    "the quick brown fox jumps over the lazy dog while a sleepy cat watches "
    "the rain from a warm windowsill";
// 67 characters: one line up to width 120, four lines at width 20.
const char kFourLineText[] = "the quick brown fox jumps over the lazy dog and then naps in the sun";
// 30 Hangul syllables, no spaces: display width 60.
const char kHangul30[] = "가나다라마바사아자차가나다라마바사아자차가나다라마바사아자차";

}

// Success criterion 4: out-of-order translations still display in seq order.
TEST_CASE("caption composer emits segments in seq order when translations arrive out of order")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(1, "S1", 0);
    composer.push_final(2, "S2", 100);
    composer.push_final(3, "S3", 200);

    composer.on_translated(3, "T3", 300);
    REQUIRE(composer.render(300) == std::nullopt);

    composer.on_translated(1, "T1", 400);
    REQUIRE(composer.render(400) == std::optional<std::string>("T1"));

    composer.on_translated(2, "T2", 500);
    REQUIRE(composer.render(500) == std::optional<std::string>("T2\nT3"));
}

// Success criterion 4: in-order arrival grows the window one segment at a time.
TEST_CASE("caption composer grows the display as in-order translations arrive")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(1, "S1", 0);
    composer.push_final(2, "S2", 10);
    composer.push_final(3, "S3", 20);

    composer.on_translated(1, "T1", 30);
    REQUIRE(composer.render(30) == std::optional<std::string>("T1"));

    composer.on_translated(2, "T2", 40);
    REQUIRE(composer.render(40) == std::optional<std::string>("T1\nT2"));

    composer.on_translated(3, "T3", 50);
    REQUIRE(composer.render(50) == std::optional<std::string>("T2\nT3"));
}

// Success criterion 7: a failed segment is skipped and unblocks later segments.
TEST_CASE("caption composer skips a failed segment without blocking later ones")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(1, "S1", 0);
    composer.push_final(2, "S2", 10);
    composer.push_final(3, "S3", 20);

    composer.on_translated(1, "T1", 30);
    REQUIRE(composer.render(30) == std::optional<std::string>("T1"));

    composer.on_translated(3, "T3", 40);
    REQUIRE(composer.render(40) == std::nullopt);

    composer.on_failed(2, "http 503", 50);
    REQUIRE(composer.render(50) == std::optional<std::string>("T1\nT3"));

    composer.push_final(4, "S4", 60);
    composer.on_translated(4, "T4", 70);
    REQUIRE(composer.render(70) == std::optional<std::string>("T3\nT4"));
}

TEST_CASE("caption composer drops the whole window when every segment fails")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(1, "S1", 0);
    composer.push_final(2, "S2", 10);
    composer.on_failed(1, "timeout", 20);
    composer.on_failed(2, "timeout", 30);

    REQUIRE(composer.render(30) == std::nullopt);
    REQUIRE(composer.pending_count() == 0);
}

// Success criterion 3: the display is cleared once after the hold timeout.
TEST_CASE("caption composer clears the display exactly once after the hold timeout")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(1, "S1", 0);
    composer.on_translated(1, "T1", 0);
    REQUIRE(composer.render(0) == std::optional<std::string>("T1"));

    REQUIRE(composer.render(3999) == std::nullopt);
    REQUIRE(composer.render(4000) == std::optional<std::string>(""));
    REQUIRE(composer.render(5000) == std::nullopt);

    composer.push_final(2, "S2", 6000);
    composer.on_translated(2, "T2", 6000);
    REQUIRE(composer.render(6000) == std::optional<std::string>("T2"));

    REQUIRE(composer.render(9999) == std::nullopt);
    REQUIRE(composer.render(10000) == std::optional<std::string>(""));
}

// Success criterion 3: a new emission before the timeout extends the hold.
TEST_CASE("caption composer extends the hold timer on a new emission")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(1, "S1", 0);
    composer.on_translated(1, "T1", 0);
    REQUIRE(composer.render(0) == std::optional<std::string>("T1"));

    composer.push_final(2, "S2", 3000);
    composer.on_translated(2, "T2", 3000);
    REQUIRE(composer.render(3000) == std::optional<std::string>("T1\nT2"));

    REQUIRE(composer.render(4000) == std::nullopt);
    REQUIRE(composer.render(6999) == std::nullopt);
    REQUIRE(composer.render(7000) == std::optional<std::string>(""));
}

TEST_CASE("caption composer does not re-show segments emitted before the hold timeout")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(1, "S1", 0);
    composer.on_translated(1, "T1", 0);
    REQUIRE(composer.render(0) == std::optional<std::string>("T1"));
    REQUIRE(composer.render(4000) == std::optional<std::string>(""));

    composer.push_final(2, "S2", 5000);
    composer.on_translated(2, "T2", 5000);
    REQUIRE(composer.render(5000) == std::optional<std::string>("T2"));
}

TEST_CASE("caption composer with a window of one shows only the newest segment")
{
    CaptionComposer composer(make_config(1, 4000));

    composer.push_final(1, "S1", 0);
    composer.push_final(2, "S2", 10);
    composer.on_translated(1, "T1", 20);
    composer.on_translated(2, "T2", 20);

    REQUIRE(composer.render(20) == std::optional<std::string>("T2"));
}

TEST_CASE("caption composer applies a smaller window on the next render")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(1, "S1", 0);
    composer.push_final(2, "S2", 10);
    composer.on_translated(1, "T1", 20);
    composer.on_translated(2, "T2", 20);
    REQUIRE(composer.render(20) == std::optional<std::string>("T1\nT2"));

    composer.set_config(make_config(1, 4000));
    REQUIRE(composer.render(30) == std::optional<std::string>("T2"));
    REQUIRE(composer.render(40) == std::nullopt);
}

TEST_CASE("caption composer reports no change when nothing happened")
{
    CaptionComposer composer;

    REQUIRE(composer.render(0) == std::nullopt);
    REQUIRE(composer.render(1000) == std::nullopt);

    composer.push_final(1, "S1", 0);
    REQUIRE(composer.render(1000) == std::nullopt);

    composer.on_translated(1, "T1", 1000);
    REQUIRE(composer.render(1000) == std::optional<std::string>("T1"));
    REQUIRE(composer.render(1000) == std::nullopt);
}

TEST_CASE("caption composer ignores outcomes for unknown segments")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.on_translated(99, "ghost", 0);
    composer.on_failed(42, "ghost", 0);
    REQUIRE(composer.render(0) == std::nullopt);

    composer.push_final(1, "S1", 10);
    composer.on_translated(1, "T1", 20);
    REQUIRE(composer.render(20) == std::optional<std::string>("T1"));

    // Already emitted: the seq is no longer known.
    composer.on_translated(1, "T1-again", 30);
    REQUIRE(composer.render(30) == std::nullopt);
}

TEST_CASE("caption composer keeps the first outcome for a segment")
{
    CaptionComposer composer(make_config(4, 4000));

    composer.push_final(1, "S1", 0);
    composer.on_translated(1, "first", 10);
    composer.on_translated(1, "second", 20);
    composer.on_failed(1, "late failure", 30);
    REQUIRE(composer.render(30) == std::optional<std::string>("first"));

    composer.push_final(2, "S2", 40);
    composer.on_failed(2, "http 429", 50);
    composer.on_translated(2, "too late", 60);
    REQUIRE(composer.render(60) == std::nullopt);
}

TEST_CASE("caption composer ignores a non-increasing seq")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(2, "S2", 0);
    composer.push_final(1, "S1", 10);
    REQUIRE(composer.pending_count() == 1);
    REQUIRE(composer.context(4) == std::vector<std::string>{"S2"});

    composer.on_translated(1, "T1", 20);
    REQUIRE(composer.render(20) == std::nullopt);

    composer.on_translated(2, "T2", 30);
    REQUIRE(composer.render(30) == std::optional<std::string>("T2"));
}

TEST_CASE("caption composer returns source context oldest first and capped")
{
    CaptionComposer composer(make_config(2, 4000));

    REQUIRE(composer.context(2).empty());

    composer.push_final(1, "one", 0);
    composer.push_final(2, "two", 10);
    composer.push_final(3, "three", 20);

    REQUIRE(composer.context(0).empty());
    REQUIRE(composer.context(2) == std::vector<std::string>{"two", "three"});
    REQUIRE(composer.context(10) == std::vector<std::string>{"one", "two", "three"});

    // Resolved segments stay available as context.
    composer.on_translated(1, "T1", 30);
    composer.on_failed(2, "boom", 30);
    composer.render(30);
    REQUIRE(composer.context(10) == std::vector<std::string>{"one", "two", "three"});
}

TEST_CASE("caption composer keeps at most 16 source texts for context")
{
    CaptionComposer composer(make_config(2, 4000));

    for (uint64_t seq = 1; seq <= 20; ++seq)
        composer.push_final(seq, "s" + std::to_string(seq), seq);

    auto ctx = composer.context(100);
    REQUIRE(ctx.size() == 16);
    REQUIRE(ctx.front() == "s5");
    REQUIRE(ctx.back() == "s20");
}

TEST_CASE("caption composer counts unresolved segments")
{
    CaptionComposer composer(make_config(2, 4000));

    REQUIRE(composer.pending_count() == 0);

    composer.push_final(1, "S1", 0);
    composer.push_final(2, "S2", 10);
    composer.push_final(3, "S3", 20);
    REQUIRE(composer.pending_count() == 3);

    composer.on_translated(3, "T3", 30);
    REQUIRE(composer.pending_count() == 2);

    composer.on_translated(1, "T1", 40);
    REQUIRE(composer.pending_count() == 1);

    composer.render(40);
    REQUIRE(composer.pending_count() == 1);

    composer.on_failed(2, "boom", 50);
    REQUIRE(composer.pending_count() == 0);

    composer.render(50);
    REQUIRE(composer.pending_count() == 0);
}

TEST_CASE("caption composer clear drops everything and reports the empty display once")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(1, "S1", 0);
    composer.push_final(2, "S2", 10);
    composer.on_translated(1, "T1", 20);
    REQUIRE(composer.render(20) == std::optional<std::string>("T1"));

    composer.clear();
    REQUIRE(composer.pending_count() == 0);
    REQUIRE(composer.context(10).empty());
    REQUIRE(composer.render(30) == std::optional<std::string>(""));
    REQUIRE(composer.render(40) == std::nullopt);

    // Outcomes for the dropped segments are ignored.
    composer.on_translated(2, "T2", 50);
    REQUIRE(composer.render(50) == std::nullopt);

    composer.push_final(1, "S1b", 60);
    composer.on_translated(1, "T1b", 60);
    REQUIRE(composer.render(60) == std::optional<std::string>("T1b"));
}

TEST_CASE("caption composer clear on an empty display reports no change")
{
    CaptionComposer composer(make_config(2, 4000));

    composer.push_final(1, "S1", 0);
    composer.clear();
    REQUIRE(composer.render(10) == std::nullopt);
}

// Spec 002 §4.3: max_lines 1-6, max_width 10-120, hold_ms 1000-30000.
TEST_CASE("caption composer clamps the configuration")
{
    CaptionComposer low(make_config(0, 4, 10));
    REQUIRE(low.config().max_lines == 1);
    REQUIRE(low.config().max_width == 10);
    REQUIRE(low.config().hold_ms == 1000);

    CaptionComposer high(make_config(9, 500, 600000));
    REQUIRE(high.config().max_lines == 6);
    REQUIRE(high.config().max_width == 120);
    REQUIRE(high.config().hold_ms == 30000);

    CaptionComposer composer;
    REQUIRE(composer.config().max_lines == 2);
    REQUIRE(composer.config().max_width == 60);
    REQUIRE(composer.config().hold_ms == 4000);

    composer.set_config(make_config(-3, -1, 0));
    REQUIRE(composer.config().max_lines == 1);
    REQUIRE(composer.config().max_width == 10);
    REQUIRE(composer.config().hold_ms == 1000);

    composer.set_config(make_config(6, 120, 30000));
    REQUIRE(composer.config().max_lines == 6);
    REQUIRE(composer.config().max_width == 120);
    REQUIRE(composer.config().hold_ms == 30000);
}

TEST_CASE("caption composer honours a clamped hold timeout")
{
    CaptionComposer composer(make_config(2, 100));

    composer.push_final(1, "S1", 0);
    composer.on_translated(1, "T1", 0);
    REQUIRE(composer.render(0) == std::optional<std::string>("T1"));

    REQUIRE(composer.render(999) == std::nullopt);
    REQUIRE(composer.render(1000) == std::optional<std::string>(""));
}

// Success criterion 7: the window holds lines, not segments. A two-line segment
// pushes the previous one-line segment off screen.
TEST_CASE("caption composer keeps a window of lines across segments")
{
    const std::vector<std::string> wrapped = wrap_text(kTwoLineText, 60);
    REQUIRE(wrapped.size() == 2);

    CaptionComposer composer(make_config(2, 60, 4000));

    composer.push_final(1, "SA", 0);
    composer.on_translated(1, "A", 0);
    REQUIRE(composer.render(0) == std::optional<std::string>("A"));

    composer.push_final(2, "SB", 10);
    composer.on_translated(2, kTwoLineText, 10);
    REQUIRE(composer.render(10) ==
            std::optional<std::string>(wrapped[0] + "\n" + wrapped[1]));

    composer.push_final(3, "SC", 20);
    composer.on_translated(3, "C", 20);
    REQUIRE(composer.render(20) == std::optional<std::string>(wrapped[1] + "\nC"));

    REQUIRE(composer.take_truncations().empty());
}

// Success criterion 8: a segment longer than the window is cut with an ellipsis
// and reported through take_truncations().
TEST_CASE("caption composer truncates a segment that overflows the window")
{
    REQUIRE(wrap_text(kFourLineText, 20).size() == 4);

    CaptionComposer composer(make_config(2, 20, 4000));

    composer.push_final(7, "S7", 0);
    composer.on_translated(7, kFourLineText, 0);

    const auto display = composer.render(0);
    REQUIRE(display.has_value());

    const std::vector<std::string> shown = split_lines(*display);
    REQUIRE(shown.size() == 2);
    REQUIRE(display_width(shown[1]) <= 20);
    REQUIRE(shown[1].size() >= 3);
    REQUIRE(shown[1].substr(shown[1].size() - 3) == "\xE2\x80\xA6");

    const std::vector<CaptionTruncation> cuts = composer.take_truncations();
    REQUIRE(cuts.size() == 1);
    REQUIRE(cuts[0].seq == 7);
    REQUIRE(cuts[0].lines == 4);
    REQUIRE(cuts[0].kept == 2);

    // The list is cleared on read.
    REQUIRE(composer.take_truncations().empty());
}

// Success criterion 8 with CJK: widths are display units, so 30 Hangul
// syllables need two lines of width 40 and one of them is cut.
TEST_CASE("caption composer truncates a Hangul segment on display width")
{
    REQUIRE(display_width(kHangul30) == 60);
    REQUIRE(wrap_text(kHangul30, 40).size() == 2);

    CaptionComposer composer(make_config(1, 40, 4000));

    composer.push_final(3, "S3", 0);
    composer.on_translated(3, kHangul30, 0);

    const auto display = composer.render(0);
    REQUIRE(display.has_value());
    REQUIRE(split_lines(*display).size() == 1);
    REQUIRE(display_width(*display) <= 40);
    REQUIRE(display->substr(display->size() - 3) == "\xE2\x80\xA6");

    const std::vector<CaptionTruncation> cuts = composer.take_truncations();
    REQUIRE(cuts.size() == 1);
    REQUIRE(cuts[0].seq == 3);
    REQUIRE(cuts[0].lines == 2);
    REQUIRE(cuts[0].kept == 1);
}

// Success criterion 13: max_lines 1 at width 120 is a single-line caption.
TEST_CASE("caption composer with one line and a wide box shows only the newest line")
{
    CaptionComposer composer(make_config(1, 120, 4000));

    composer.push_final(1, "S1", 0);
    composer.push_final(2, "S2", 10);
    composer.push_final(3, "S3", 20);

    composer.on_translated(1, "T1", 30);
    REQUIRE(composer.render(30) == std::optional<std::string>("T1"));

    composer.on_translated(2, "T2", 40);
    composer.on_translated(3, "T3", 40);
    REQUIRE(composer.render(40) == std::optional<std::string>("T3"));
    REQUIRE(composer.take_truncations().empty());
}

TEST_CASE("caption composer trims the window immediately when max_lines shrinks")
{
    CaptionComposer composer(make_config(3, 60, 4000));

    composer.push_final(1, "S1", 0);
    composer.push_final(2, "S2", 10);
    composer.push_final(3, "S3", 20);
    composer.on_translated(1, "T1", 30);
    composer.on_translated(2, "T2", 30);
    composer.on_translated(3, "T3", 30);
    REQUIRE(composer.render(30) == std::optional<std::string>("T1\nT2\nT3"));

    composer.set_config(make_config(1, 60, 4000));
    REQUIRE(composer.render(40) == std::optional<std::string>("T3"));
    REQUIRE(composer.render(50) == std::nullopt);
}

TEST_CASE("caption composer does not re-wrap lines already on screen when max_width changes")
{
    CaptionComposer composer(make_config(2, 120, 4000));

    composer.push_final(1, "S1", 0);
    composer.on_translated(1, kFourLineText, 0);
    REQUIRE(composer.render(0) == std::optional<std::string>(kFourLineText));

    // The narrower box applies to the next segment only.
    composer.set_config(make_config(2, 20, 4000));
    REQUIRE(composer.render(10) == std::nullopt);

    composer.push_final(2, "S2", 20);
    composer.on_translated(2, kFourLineText, 20);

    const auto display = composer.render(20);
    REQUIRE(display.has_value());
    const std::vector<std::string> shown = split_lines(*display);
    REQUIRE(shown.size() == 2);
    REQUIRE(shown[0] == "the quick brown fox");
    REQUIRE(display_width(shown[1]) <= 20);

    const std::vector<CaptionTruncation> cuts = composer.take_truncations();
    REQUIRE(cuts.size() == 1);
    REQUIRE(cuts[0].seq == 2);
    REQUIRE(cuts[0].lines == 4);
    REQUIRE(cuts[0].kept == 2);
}

TEST_CASE("caption composer ignores a whitespace-only translation")
{
    CaptionComposer composer(make_config(2, 60, 4000));

    composer.push_final(1, "S1", 0);
    composer.on_translated(1, "T1", 0);
    REQUIRE(composer.render(0) == std::optional<std::string>("T1"));

    composer.push_final(2, "S2", 1000);
    composer.on_translated(2, "   ", 1000);
    REQUIRE(composer.render(1000) == std::nullopt);
    REQUIRE(composer.pending_count() == 0);
    REQUIRE(composer.take_truncations().empty());

    // The hold timer still runs from the last real emission at t=0.
    REQUIRE(composer.render(3999) == std::nullopt);
    REQUIRE(composer.render(4000) == std::optional<std::string>(""));
}

TEST_CASE("caption composer clear drops pending truncations")
{
    CaptionComposer composer(make_config(1, 20, 4000));

    composer.push_final(1, "S1", 0);
    composer.on_translated(1, kFourLineText, 0);
    REQUIRE(composer.render(0).has_value());

    composer.clear();
    REQUIRE(composer.take_truncations().empty());
    REQUIRE(composer.render(10) == std::optional<std::string>(""));
}
