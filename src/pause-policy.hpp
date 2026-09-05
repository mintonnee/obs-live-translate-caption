#pragma once
#include <cstdint>

// Idle detection and pause-reason resolution for the caption session
// (docs/specs/004-idle-pause-and-output-gating.md §4.2). Pure logic: no libobs,
// no threads, no clock — callers pass timestamps in.
namespace lt {

// 16-bit PCM RMS for a level in dBFS, with RMS 32767 as 0 dBFS:
// rms = 32767 * 10^(dbfs / 20). dbfs_to_rms(-45) ~= 184.
double dbfs_to_rms(double dbfs);

// Tracks how long the input has been below the signal threshold.
//
// feed(has_signal, now) is called once per audio chunk. A signal chunk records
// `now` as the last signal time and returns false. A silent chunk returns true
// iff a timeout is configured and now - last_signal >= timeout. The very first
// poll(now) also advances the timer without an audio chunk. The first feed or
// poll after construction establishes the reference point; reset(now) establishes
// it immediately. Connection changes must not reset this audio-based timer.
//
// start_idle(now) is the session-start rule (spec 004 §4.4): the detector is
// idle right away and stays so through silence until the first signal chunk,
// so a fresh session does not connect before anyone has spoken. It is a no-op
// when no timeout is configured.
class IdleDetector {
public:
    void configure(uint64_t timeout_ms); // 0 = never idle
    bool feed(bool has_signal, uint64_t now_ms);
    bool poll(uint64_t now_ms); // worker tick, including when audio stops arriving
    void reset(uint64_t now_ms); // explicit new window (stop / output resume)
    void start_idle(uint64_t now_ms); // idle until the first signal chunk
    bool idle() const;           // result of the last feed()
    bool awaiting_signal() const; // start_idle() armed and no signal seen yet
    uint64_t timeout_ms() const;

private:
    uint64_t timeout_ms_ = 0;
    uint64_t last_signal_ms_ = 0;
    bool started_ = false;
    bool idle_ = false;
    bool awaiting_signal_ = false;
};

enum class PauseReason { None, OutputInactive, Idle };

struct PauseInputs {
    bool only_while_output = false; // config: only_while_output_active
    bool output_active = false;     // streaming || recording || virtualcam
    bool idle = false;              // IdleDetector::idle()
};

// OutputInactive wins over Idle (spec criterion 6).
PauseReason resolve_pause(const PauseInputs &in);

// "none" / "output inactive" / "idle" — for status text and log lines.
const char *pause_reason_name(PauseReason r);

}
