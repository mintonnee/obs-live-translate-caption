#include <catch2/catch_test_macros.hpp>
#include "caption-composer.hpp"

#include <string>
#include <vector>

using namespace lt;

namespace {

CaptionComposerConfig make_config(int max_segments, uint64_t hold_ms)
{
    CaptionComposerConfig cfg;
    cfg.max_segments = max_segments;
    cfg.hold_ms = hold_ms;
    return cfg;
}

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

TEST_CASE("caption composer clamps the configuration")
{
    CaptionComposer low(make_config(0, 10));
    REQUIRE(low.config().max_segments == 1);
    REQUIRE(low.config().hold_ms == 1000);

    CaptionComposer high(make_config(9, 600000));
    REQUIRE(high.config().max_segments == 4);
    REQUIRE(high.config().hold_ms == 30000);

    CaptionComposer composer;
    REQUIRE(composer.config().max_segments == 2);
    REQUIRE(composer.config().hold_ms == 4000);

    composer.set_config(make_config(-3, 0));
    REQUIRE(composer.config().max_segments == 1);
    REQUIRE(composer.config().hold_ms == 1000);

    composer.set_config(make_config(4, 30000));
    REQUIRE(composer.config().max_segments == 4);
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
