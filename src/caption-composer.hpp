#pragma once
#include "caption-segment.hpp"
#include <deque>

namespace lt {

struct CaptionComposerConfig {
    int max_lines = 2;       // 1-6 lines in the rolling display window
    int max_width = 60;      // 10-120 display units
    uint64_t hold_ms = 4000; // 1000-30000 ms from the latest visible page/update
};

// Legacy adapter only. Normal pagination never truncates.
struct CaptionTruncation {
    uint64_t seq = 0;
    int lines = 0;
    int kept = 0;
};

enum class CaptionComposeReject {
    None, InvalidId, StaleGeneration, ReusedId, MissingSegment, RetiredSegment,
    StaleRevision, StaleAttempt, Duplicate, RevisionConflict, Expired,
    ResourceLimit, EmptyTranslation, ReconcileAmbiguous
};
enum class CaptionDisplayEventKind { PageEntered, PageReplaced, Retired, RemainderDiscarded };

struct CaptionDisplayEvent {
    CaptionDisplayEventKind kind = CaptionDisplayEventKind::Retired;
    SegmentKey key;
    CaptionRetireReason reason = CaptionRetireReason::None;
    size_t page_index = 0;
    size_t discarded_pages = 0;
    uint64_t at_ms = 0;
};
struct CaptionComposeResult {
    bool accepted = false;
    CaptionComposeReject reject = CaptionComposeReject::None;
    std::vector<CaptionDisplayEvent> events;
};
struct CaptionSnapshot {
    uint64_t publication = 0; // Monotonic across resets; serialize sink publication.
    std::optional<SegmentKey> key;
    TranslationJobId job;
    CaptionDisplayStatus display;
    std::string text;
};
struct CaptionRenderResult {
    std::optional<std::string> changed; // Includes identity changes with identical text.
    CaptionSnapshot snapshot;
    std::vector<CaptionDisplayEvent> events;
};

// Pure, thread-unsafe reducer. Under one session lock, apply all completions then
// tick once. Only tick appends a new page to the rolling line window. Publish its
// snapshot outside that lock.
class CaptionComposer {
public:
    static constexpr size_t kMaxManaged = 256;
    static constexpr size_t kMaxWaiting = 24;
    static constexpr size_t kMaxTranslationBytes = 16 * 1024;
    static constexpr uint64_t kStaleBeforeDisplayMs = 6000;
    // Caps undisplayed remaining pages, but never shortens the final page's hold.
    static constexpr uint64_t kSegmentLifetimeMs = 15000;

    explicit CaptionComposer(CaptionComposerConfig cfg = {});
    CaptionComposeResult reset_generation(uint64_t generation, uint64_t now_ms);
    CaptionComposeResult register_segment(const CaptionSegment &segment, uint64_t now_ms);
    // Register a replacement group atomically before injecting completions.
    CaptionComposeResult register_segments(const std::vector<CaptionSegment> &segments,
                                           uint64_t now_ms);
    // A final transcript may revise or invalidate its visible interim. Keep the
    // existing snapshot for one hold interval while reconciliation runs.
    CaptionComposeResult confirm_final(const SegmentKey &visible_key, uint64_t now_ms);
    CaptionComposeResult expect_job(const TranslationJobId &id, uint64_t now_ms);
    CaptionComposeResult complete(const TranslationCompletion &completion, uint64_t now_ms);
    CaptionComposeResult retire(const SegmentKey &key, CaptionRetireReason reason, uint64_t now_ms);
    CaptionRenderResult tick(uint64_t now_ms);
    CaptionSnapshot snapshot() const { return published_; }
    std::optional<CaptionDisplayStatus> display_status(const SegmentKey &key) const;
    bool forget_retired(const SegmentKey &key);
    size_t managed_count() const { return segments_.size(); }
    size_t waiting_count() const;

    CaptionComposeResult set_config(const CaptionComposerConfig &cfg, uint64_t now_ms);
    void set_config(const CaptionComposerConfig &cfg); // Deferred until next tick.
    CaptionComposerConfig config() const;

    // Pre-005 session compatibility only. seq must increase across clear; these
    // callbacks carry no generation, so new integrations must use typed IDs above.
    void push_final(uint64_t seq, const std::string &source_text, uint64_t now_ms);
    void on_translated(uint64_t seq, const std::string &translated_text, uint64_t now_ms);
    void on_failed(uint64_t seq, const std::string &reason, uint64_t now_ms);
    std::optional<std::string> render(uint64_t now_ms);
    std::vector<CaptionTruncation> take_truncations(); // Always empty; use events.
    std::vector<std::string> context(size_t max) const;
    size_t pending_count() const;
    void clear();

private:
    struct Page { std::string text; size_t end_byte = 0; };
    struct Segment {
        CaptionSegment source;
        TranslationJobId expected;
        TranslationJobId translated_job;
        CaptionDisplayStatus status;
        uint64_t registered_ms = 0;
        bool processed = false;
        bool ambiguous_prefix = false;
        std::optional<SegmentKey> anchor;
        std::string text;
        size_t consumed_bytes = 0;
        std::deque<Page> pages;
    };
    struct Slot {
        SegmentKey key;
        TranslationJobId job;
        CaptionDisplayStatus status;
        uint64_t hold_started_ms = 0; // Display update or final confirmation.
        bool final_confirmed = false;
        std::string text;
        std::string completed_prefix;
        bool ambiguous_prefix = false;
        bool placeholder = false;
        bool replacement_dirty = false;
        bool retire_after_minimum = false;
    };
    struct WindowChunk {
        SegmentKey key;
        std::string text;
        bool starts_page = false; // Preserve pagination, not translation boundaries.
    };
    Segment *find(const SegmentKey &key);
    const Segment *find(const SegmentKey &key) const;
    std::deque<Page> paginate(const std::string &text, size_t begin) const;
    void confirm_visible_final(uint64_t now_ms);
    void retire_segment(Segment &segment, CaptionRetireReason reason, uint64_t now_ms,
                        std::vector<CaptionDisplayEvent> &events);
    void expire(uint64_t now_ms, std::vector<CaptionDisplayEvent> &events);
    void clear_slot(CaptionRetireReason reason, uint64_t now_ms,
                    std::vector<CaptionDisplayEvent> &events,
                    bool preserve_window = false);
    std::string window_text() const;
    void replace_window_chunk(const SegmentKey &old_key, const SegmentKey &new_key,
                              const std::string &text, bool starts_page);
    void update_visible(Segment &segment, std::string text, uint64_t now_ms,
                        std::vector<CaptionDisplayEvent> &events);
    void enter(Segment &segment, uint64_t now_ms,
               std::vector<CaptionDisplayEvent> &events, bool inherit = false);
    Segment *next_waiting() const;
    Segment *replacement_head() const;
    void enforce_waiting(uint64_t now_ms, std::vector<CaptionDisplayEvent> &events);
    void apply_config(const CaptionComposerConfig &cfg, uint64_t now_ms,
                      std::vector<CaptionDisplayEvent> &events);
    uint64_t generation_ = 0;
    uint64_t high_water_ = 0;
    uint64_t last_now_ms_ = 0;
    CaptionComposerConfig cfg_;
    std::optional<CaptionComposerConfig> pending_config_;
    std::vector<Segment> segments_;
    std::optional<Slot> slot_;
    std::deque<WindowChunk> window_;
    CaptionSnapshot published_;
    std::deque<std::string> legacy_sources_;
};

}
