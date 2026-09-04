#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Orders translated segments, keeps the on-screen window, and clears it after a
// hold timeout. Pure logic: every method takes the clock (`now_ms`) as an
// argument so tests can drive time. Not thread-safe; the caption session
// serializes calls with its own mutex.
// The window is a list of wrapped lines bounded by `max_lines` x `max_width`
// display units, so the OBS text source never grows past its box.
// Spec: docs/specs/001-caption-translation-pipeline.md §4.5, criteria 3, 4, 7;
// docs/specs/002-caption-text-box-limits.md §4.3, criteria 7, 8, 13.
namespace lt {

struct CaptionComposerConfig {
    int max_lines = 2;        // lines kept on screen (1-6)
    int max_width = 60;       // line width in display units (10-120), see caption-wrap.hpp
    uint64_t hold_ms = 4000;  // clear the display this long after the last emission
};

// A translated segment whose wrapped text did not fit into `max_lines` and was
// cut short by render(). The session logs these (spec 002 criterion 8).
struct CaptionTruncation {
    uint64_t seq = 0;  // segment that was cut
    int lines = 0;     // wrapped line count before cutting
    int kept = 0;      // lines kept (== max_lines at the time)
};

class CaptionComposer {
public:
    explicit CaptionComposer(CaptionComposerConfig cfg = {});

    // Takes effect on the next emitted segment. A smaller `max_lines` drops
    // lines from the front of the window immediately; a changed `max_width`
    // does not re-wrap lines that are already on screen.
    void set_config(const CaptionComposerConfig &cfg);
    CaptionComposerConfig config() const;

    // Register a finalized source segment. `seq` is strictly increasing per
    // session; segments are emitted in `seq` order regardless of the order in
    // which on_translated()/on_failed() arrive.
    void push_final(uint64_t seq, const std::string &source_text, uint64_t now_ms);

    // Translation outcome for `seq`. Unknown seqs are ignored. A failed segment
    // is skipped (never displayed) but no longer blocks later segments.
    void on_translated(uint64_t seq, const std::string &translated_text, uint64_t now_ms);
    void on_failed(uint64_t seq, const std::string &reason, uint64_t now_ms);

    // Returns the display string when it changed since the previous render()
    // call, std::nullopt when unchanged. Emits every leading segment whose
    // outcome is known (in seq order), wrapping each to `max_width` and
    // appending its lines to the window. Returns "" exactly once when the
    // display is non-empty and now_ms >= last emission time + hold_ms; a later
    // emission restarts the timer.
    std::optional<std::string> render(uint64_t now_ms);

    // Truncations recorded by render() since the last call; cleared on return.
    std::vector<CaptionTruncation> take_truncations();

    // Most recent `max` pushed source texts (any state), oldest first. Used as
    // translation context; call it *before* push_final() for the new segment.
    std::vector<std::string> context(size_t max) const;

    // Segments pushed but not yet resolved (translated or failed).
    size_t pending_count() const;

    // Forget everything; the next render() reports "" if something was shown.
    void clear();

private:
    enum class SegmentState { Pending, Translated, Failed };

    struct Segment {
        SegmentState state = SegmentState::Pending;
        std::string text;  // translated text once state == Translated
    };

    // Bound on the source texts kept for context().
    static constexpr size_t kMaxContext = 16;

    // Wraps `text`, cuts it to `max_lines` (recording a truncation) and appends
    // the lines to the window. Returns false when there is nothing to show.
    bool emit_segment(uint64_t seq, const std::string &text);
    void trim_window();
    std::string build_display() const;

    CaptionComposerConfig cfg_;
    std::map<uint64_t, Segment> segments_;  // pushed, not yet emitted/skipped
    std::deque<std::string> lines_;         // on-screen lines, oldest first
    std::deque<std::string> sources_;       // pushed source texts, oldest first
    std::vector<CaptionTruncation> truncations_;  // since the last take_truncations()
    std::string last_display_;                    // what the previous render() showed
    uint64_t last_emit_ms_ = 0;
    uint64_t last_seq_ = 0;
    bool has_pushed_ = false;
};

}
