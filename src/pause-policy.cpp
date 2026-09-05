#include "pause-policy.hpp"

#include <cmath>

// S1: idle detection and pause-reason resolution
// (docs/specs/004-idle-pause-and-output-gating.md §4.2, criteria 1-6).
namespace lt {

double dbfs_to_rms(double dbfs) { return 32767.0 * std::pow(10.0, dbfs / 20.0); }

void IdleDetector::configure(uint64_t timeout_ms) { timeout_ms_ = timeout_ms; }

bool IdleDetector::feed(bool has_signal, uint64_t now_ms)
{
    if (has_signal) {
        last_signal_ms_ = now_ms;
        idle_ = false;
        awaiting_signal_ = false;
        return false;
    }

    if (!started_) {
        // First feed after construction/reset is the reference point: it
        // never reports idle by itself.
        started_ = true;
        last_signal_ms_ = now_ms;
        idle_ = false;
        return idle_;
    }

    const uint64_t elapsed = now_ms >= last_signal_ms_ ? now_ms - last_signal_ms_ : 0;
    // While awaiting the first signal the timer is irrelevant: silence keeps
    // the detector idle. Disabling the timeout (0) still wins, so criterion 12
    // resumes a session that is only waiting for sound.
    idle_ = timeout_ms_ > 0 && (awaiting_signal_ || elapsed >= timeout_ms_);
    return idle_;
}

void IdleDetector::reset(uint64_t now_ms)
{
    last_signal_ms_ = now_ms;
    started_ = true;
    idle_ = false;
    awaiting_signal_ = false;
}

void IdleDetector::start_idle(uint64_t now_ms)
{
    last_signal_ms_ = now_ms;
    started_ = true;
    awaiting_signal_ = timeout_ms_ > 0;
    idle_ = awaiting_signal_;
}

bool IdleDetector::idle() const { return idle_; }

bool IdleDetector::awaiting_signal() const { return awaiting_signal_; }

uint64_t IdleDetector::timeout_ms() const { return timeout_ms_; }

PauseReason resolve_pause(const PauseInputs &in)
{
    if (in.only_while_output && !in.output_active)
        return PauseReason::OutputInactive;
    if (in.idle)
        return PauseReason::Idle;
    return PauseReason::None;
}

const char *pause_reason_name(PauseReason r)
{
    switch (r) {
    case PauseReason::OutputInactive:
        return "output inactive";
    case PauseReason::Idle:
        return "idle";
    case PauseReason::None:
    default:
        return "none";
    }
}

}
