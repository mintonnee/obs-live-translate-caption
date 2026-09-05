#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>
#include "pause-policy.hpp"

// Tests for spec 004-idle-pause-and-output-gating.md, criteria 1-6.
// Silence is fed in 100 ms chunks, matching the real audio chunk cadence.

using lt::IdleDetector;
using lt::PauseInputs;
using lt::PauseReason;
using lt::dbfs_to_rms;
using lt::pause_reason_name;
using lt::resolve_pause;

namespace {
constexpr uint64_t kChunkMs = 100;

// Feed silent chunks from t=from_ms (exclusive of from_ms itself already fed)
// up to and including t=to_ms, stepping by kChunkMs, returning the detector's
// idle() after the last chunk.
bool feed_silence_until(IdleDetector &d, uint64_t from_ms, uint64_t to_ms)
{
    bool idle = d.idle();
    for (uint64_t t = from_ms; t <= to_ms; t += kChunkMs)
        idle = d.feed(false, t);
    return idle;
}
}

TEST_CASE("dbfs_to_rms matches spec reference points") // criterion 1
{
    REQUIRE(dbfs_to_rms(0.0) == Catch::Approx(32767.0).margin(1.0));
    REQUIRE(dbfs_to_rms(-45.0) == Catch::Approx(184.0).margin(1.0));
    REQUIRE(dbfs_to_rms(-90.0) == Catch::Approx(1.0).margin(0.1));
}

TEST_CASE("first feed after construction is not idle even at a huge timestamp")
{
    IdleDetector d;
    d.configure(300000);
    REQUIRE_FALSE(d.feed(false, 1000000000ULL));
    REQUIRE_FALSE(d.idle());
}

TEST_CASE("reset restarts the timer from its own now, not from the next feed")
{
    // Per the contract, reset(now) sets last_signal_ms_ = now and
    // started_ = true directly (unlike the post-construction reference-point
    // rule), so elapsed time is measured from reset's `now` immediately.
    IdleDetector d;
    d.configure(300000);
    d.feed(true, 0);
    d.reset(5000);
    REQUIRE_FALSE(d.idle());
    REQUIRE(d.feed(false, 1000000000ULL));
    REQUIRE(d.idle());
}

TEST_CASE("300s timeout: idle right at the boundary, not before") // criterion 2
{
    IdleDetector d;
    d.configure(300000);
    d.feed(false, 0); // reference point

    REQUIRE_FALSE(feed_silence_until(d, 100, 299900));
    REQUIRE_FALSE(d.idle());

    REQUIRE(d.feed(false, 300000));
    REQUIRE(d.idle());
}

TEST_CASE("a signal chunk resets the timer") // criterion 3
{
    IdleDetector d;
    d.configure(300000);
    d.feed(false, 0); // reference point

    REQUIRE_FALSE(feed_silence_until(d, 100, 200000));

    REQUIRE_FALSE(d.feed(true, 200100));
    REQUIRE_FALSE(d.idle());

    REQUIRE_FALSE(feed_silence_until(d, 200200, 400100)); // +200s silence from reset point
    REQUIRE_FALSE(d.idle());

    REQUIRE(feed_silence_until(d, 400200, 500100)); // +100s more -> idle
    REQUIRE(d.idle());
}

TEST_CASE("timeout 0 never goes idle") // criterion 4
{
    IdleDetector d;
    d.configure(0);
    d.feed(false, 0); // reference point

    REQUIRE_FALSE(feed_silence_until(d, 100, 3600000));
    REQUIRE_FALSE(d.idle());
}

TEST_CASE("a single signal chunk clears idle immediately, and reset restarts the timer") // criterion 5
{
    IdleDetector d;
    d.configure(300000);
    d.feed(false, 0);
    REQUIRE(d.feed(false, 300000));
    REQUIRE(d.idle());

    REQUIRE_FALSE(d.feed(true, 300100));
    REQUIRE_FALSE(d.idle());

    // reset() also restarts the timer: after it, a full timeout of silence is
    // required again before idle.
    d.reset(500000);
    REQUIRE_FALSE(feed_silence_until(d, 500100, 799900));
    REQUIRE_FALSE(d.idle());
    REQUIRE(d.feed(false, 800000));
    REQUIRE(d.idle());
}

TEST_CASE("configure(0) while idle clears idle on the next feed") // criterion 12 support
{
    IdleDetector d;
    d.configure(300000);
    d.feed(false, 0);
    REQUIRE(d.feed(false, 300000));
    REQUIRE(d.idle());

    d.configure(0);
    REQUIRE_FALSE(d.feed(false, 300100));
    REQUIRE_FALSE(d.idle());
}

TEST_CASE("start_idle: idle until the first signal chunk") // criterion 5a
{
    IdleDetector d;
    d.configure(300000);
    d.start_idle(1000);
    REQUIRE(d.idle());
    REQUIRE(d.awaiting_signal());

    // Silence well short of the timeout keeps it idle.
    REQUIRE(feed_silence_until(d, 1100, 5000));
    REQUIRE(d.idle());
    REQUIRE(d.awaiting_signal());

    // One signal chunk clears both.
    REQUIRE_FALSE(d.feed(true, 5100));
    REQUIRE_FALSE(d.idle());
    REQUIRE_FALSE(d.awaiting_signal());

    // Afterwards the normal timeout applies from that signal.
    REQUIRE_FALSE(feed_silence_until(d, 5200, 305000));
    REQUIRE(d.feed(false, 305100));
}

TEST_CASE("start_idle is a no-op without a timeout") // criterion 5a
{
    IdleDetector d;
    d.configure(0);
    d.start_idle(1000);
    REQUIRE_FALSE(d.idle());
    REQUIRE_FALSE(d.awaiting_signal());
    REQUIRE_FALSE(feed_silence_until(d, 1100, 10000));
}

TEST_CASE("configure(0) after start_idle clears idle on the next feed") // criterion 5a, 12
{
    IdleDetector d;
    d.configure(300000);
    d.start_idle(1000);
    REQUIRE(d.feed(false, 1100));

    d.configure(0);
    REQUIRE_FALSE(d.feed(false, 1200));
    REQUIRE_FALSE(d.idle());
}

TEST_CASE("reset clears the start_idle wait") // criterion 5a
{
    IdleDetector d;
    d.configure(300000);
    d.start_idle(1000);
    d.reset(2000);
    REQUIRE_FALSE(d.idle());
    REQUIRE_FALSE(d.awaiting_signal());
    REQUIRE_FALSE(feed_silence_until(d, 2100, 10000));
}

TEST_CASE("resolve_pause truth table") // criterion 6
{
    REQUIRE(resolve_pause(PauseInputs{false, false, false}) == PauseReason::None);
    REQUIRE(resolve_pause(PauseInputs{true, false, false}) == PauseReason::OutputInactive);
    REQUIRE(resolve_pause(PauseInputs{false, false, true}) == PauseReason::Idle);
    REQUIRE(resolve_pause(PauseInputs{true, false, true}) == PauseReason::OutputInactive);
    REQUIRE(resolve_pause(PauseInputs{true, true, false}) == PauseReason::None);
    REQUIRE(resolve_pause(PauseInputs{true, true, true}) == PauseReason::Idle);
}

TEST_CASE("pause_reason_name for all values")
{
    REQUIRE(std::string(pause_reason_name(PauseReason::None)) == "none");
    REQUIRE(std::string(pause_reason_name(PauseReason::OutputInactive)) == "output inactive");
    REQUIRE(std::string(pause_reason_name(PauseReason::Idle)) == "idle");
}

TEST_CASE("worker polling reaches idle after audio callbacks stop")
{
    IdleDetector d;
    d.configure(300000);
    d.start_idle(0);
    d.feed(true, 1000);
    // No more feed calls: an absent source must behave like silence.
    REQUIRE_FALSE(d.poll(300999));
    REQUIRE(d.poll(301000));
    REQUIRE(d.poll(3600000));
    REQUIRE_FALSE(d.feed(true, 3600100));
    REQUIRE_FALSE(d.awaiting_signal());
    REQUIRE_FALSE(d.poll(3900099));
    REQUIRE(d.poll(3900100));
}

TEST_CASE("first signal establishes the timeout even without start or reset")
{
    IdleDetector d;
    d.configure(300000);
    REQUIRE_FALSE(d.feed(true, 1000));
    REQUIRE(d.poll(301000));
}

TEST_CASE("start wait survives worker ticks and resumes on a single signal")
{
    IdleDetector d;
    d.configure(300000);
    d.start_idle(1000);
    REQUIRE(d.poll(1100));
    REQUIRE(d.poll(3600000));
    REQUIRE(d.awaiting_signal());
    REQUIRE_FALSE(d.feed(true, 3600100));
    REQUIRE_FALSE(d.poll(3600200));
    REQUIRE_FALSE(d.awaiting_signal());
}

TEST_CASE("settings updates preserve elapsed silence across multiple rotation intervals")
{
    IdleDetector d;
    // The detector outlives WebSockets. Reapplying transport/display settings
    // must preserve a timeout longer than the nine-minute socket lifetime.
    const uint64_t timeout = GENERATE(600000ULL, 1800000ULL);
    d.configure(timeout);
    d.start_idle(0);
    d.feed(true, 1000);
    for (uint64_t now = 1100; now < timeout + 1000; now += 100) {
        if ((now - 1000) % 540000 == 0) d.configure(timeout);
        REQUIRE_FALSE(d.poll(now));
    }
    REQUIRE(d.poll(timeout + 1000));
}

TEST_CASE("timeout changes apply on a worker tick without another audio chunk")
{
    IdleDetector d;
    d.configure(300000);
    d.start_idle(0);
    d.feed(true, 1000);
    REQUIRE_FALSE(d.poll(201000));
    d.configure(120000);
    REQUIRE(d.poll(201100));
    d.configure(600000);
    REQUIRE_FALSE(d.poll(201200));
    REQUIRE(d.poll(601000));
    d.configure(0);
    REQUIRE_FALSE(d.idle());
    REQUIRE_FALSE(d.poll(3600000));
}

TEST_CASE("disabling startup wait clears it without audio")
{
    IdleDetector d;
    d.configure(300000);
    d.start_idle(0);
    d.configure(0);
    REQUIRE_FALSE(d.awaiting_signal());
    REQUIRE_FALSE(d.poll(100));
    d.configure(300000);
    REQUIRE_FALSE(d.poll(200));
    REQUIRE(d.poll(300000));
}

TEST_CASE("explicit output resume grants one new idle window without audio")
{
    IdleDetector d;
    d.configure(300000);
    d.start_idle(0);
    REQUIRE(d.poll(600000));
    d.reset(600000);
    REQUIRE_FALSE(d.awaiting_signal());
    REQUIRE_FALSE(d.poll(899999));
    REQUIRE(d.poll(900000));
}

TEST_CASE("worker polling tolerates timestamps before the reference point")
{
    IdleDetector d;
    d.configure(300000);
    d.reset(1000);
    REQUIRE_FALSE(d.poll(999));
    REQUIRE(d.poll(301000));
}
