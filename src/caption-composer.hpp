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
// Spec: docs/specs/001-caption-translation-pipeline.md §4.5, criteria 3, 4, 7.
namespace lt {

struct CaptionComposerConfig {
    int max_segments = 2;     // segments kept on screen (1-4), joined with "\n"
    uint64_t hold_ms = 4000;  // clear the display this long after the last emission
};

class CaptionComposer {
public:
    explicit CaptionComposer(CaptionComposerConfig cfg = {});

    // Takes effect on the next render(); does not drop emitted segments.
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
    // outcome is known (in seq order). Returns "" exactly once when the display
    // is non-empty and now_ms >= last emission time + hold_ms; a later emission
    // restarts the timer.
    std::optional<std::string> render(uint64_t now_ms);

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

    // Upper bound of CaptionComposerConfig::max_segments; also the number of
    // emitted texts kept so that widening the window restores earlier lines
    // until the hold timeout drops the whole window.
    static constexpr size_t kMaxSegments = 4;
    // Bound on the source texts kept for context().
    static constexpr size_t kMaxContext = 16;

    std::string build_display() const;

    CaptionComposerConfig cfg_;
    std::map<uint64_t, Segment> segments_;  // pushed, not yet emitted/skipped
    std::deque<std::string> emitted_;       // translated texts, oldest first
    std::deque<std::string> sources_;       // pushed source texts, oldest first
    std::string last_display_;              // what the previous render() showed
    uint64_t last_emit_ms_ = 0;
    uint64_t last_seq_ = 0;
    bool has_pushed_ = false;
};

}
