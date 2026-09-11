#pragma once
#include "caption-segment.hpp"

namespace lt {

enum class SchedulerRejectReason {
    None,
    InvalidId,
    StaleGeneration,
    ReusedId,
    MissingSegment,
    RetiredSegment,
    StaleRevision,
    StaleAttempt,
    RevisionConflict,
    Duplicate,
    NotDispatched,
    SourceLimit,
    ManagedLimit,
    TranslationLimit,
    InvalidUtf8,
    EmptyText,
    DisplayConflict
};

struct SchedulerRetirement {
    SegmentKey key;
    CaptionRetireReason reason = CaptionRetireReason::None;
};

// Per-transition effects, never a retained/unbounded event queue. Apply retirements
// to the composer under the caller's state lock; issue cancellations outside it.
struct SchedulerEffects {
    std::vector<SchedulerRetirement> retired;
    std::vector<TranslationJobId> cancel;
};

struct SchedulerResult {
    bool accepted = false;
    SchedulerRejectReason reject = SchedulerRejectReason::None;
    SchedulerEffects effects;
};

struct SchedulerDispatch {
    TranslationJobPtr job;
    SchedulerEffects effects;
};

struct SchedulerCompletionResult : SchedulerResult {
    TranslationOutcome outcome = TranslationOutcome::Failed;
    TranslationFailure failure = TranslationFailure::None;
};

struct ScheduledSegment {
    CaptionSegment segment;
    TranslationJobId current_job;
    CaptionDisplayState display = CaptionDisplayState::Waiting;
    CaptionRetireReason retire_reason = CaptionRetireReason::None;
    uint64_t first_registered_ms = 0;
    bool result_processed = false;
    std::string translated_text;
};

// Pure, thread-unsafe reducer. Serialize every call with the session state lock.
// dispatch reserves a real transport slot: call complete exactly when the request
// finishes, including cancellation confirmation or failure to start. Never perform
// network I/O while holding that lock. Keep this object across stop/reconnect.
class TranslationScheduler {
public:
    static constexpr size_t kMaxInFlight = 3;
    static constexpr size_t kMaxQueued = 12;
    static constexpr size_t kMaxWaiting = 24;
    static constexpr size_t kMaxManaged = 256;
    static constexpr size_t kMaxSourceBytes = 32 * 1024;
    static constexpr size_t kMaxContextSegments = 3;
    static constexpr size_t kMaxContextBytes = 2 * 1024;
    static constexpr size_t kMaxTranslationBytes = 16 * 1024;
    // Transport must enforce this while receiving AND before JSON parsing.
    static constexpr size_t kMaxResponseBytes = 64 * 1024;
    static constexpr uint64_t kStaleBeforeDisplayMs = 6000;

    // Generation and newly allocated segment_id must strictly increase across
    // resets and metadata reclamation. Order is exclusively SegmentOrderKey.
    // A fresh instance cannot check allocator history; caller owns process IDs.
    SchedulerResult reset_generation(uint64_t generation,
                                     const TranslationSettingsSnapshot &settings);
    SchedulerResult register_segment(const CaptionSegment &segment,
                                     const std::vector<std::string> &context, uint64_t now_ms);
    // Explicit 006 extension point, one attempt 2 per revision; never automatic.
    SchedulerResult request_quality_retry(const SegmentKey &key,
                                          const std::vector<std::string> &context,
                                          uint64_t now_ms);
    SchedulerDispatch dispatch(uint64_t now_ms);
    SchedulerCompletionResult complete(const TranslationCompletion &completion, uint64_t now_ms);
    SchedulerResult mark_visible(const SegmentKey &key, uint64_t now_ms);
    SchedulerResult retire(const SegmentKey &key, CaptionRetireReason reason);
    SchedulerEffects tick(uint64_t now_ms);
    // Only after caller has finished final reconciliation for this metadata.
    bool forget_retired(const SegmentKey &key);

    // Borrowed pointer invalidated by the next mutation. Do not log text fields.
    const ScheduledSegment *find(const SegmentKey &key) const;
    size_t queued_count() const { return queued_.size(); }
    size_t in_flight_count() const { return running_.size(); }
    size_t managed_count() const { return segments_.size(); }
    size_t waiting_count() const;
    uint64_t generation() const { return generation_; }

private:
    struct Running {
        TranslationJobId id;
        bool cancel_requested = false;
    };
    ScheduledSegment *find_mutable(const SegmentKey &key);
    void retire_segment(ScheduledSegment &segment, CaptionRetireReason reason,
                        SchedulerEffects &effects);
    void cancel_running(const SegmentKey &key, SchedulerEffects &effects);
    void enqueue(ScheduledSegment &segment, TranslationRequestKind kind,
                 const std::vector<std::string> &context, uint64_t now_ms,
                 SchedulerEffects &effects);
    void enforce_limits(SchedulerEffects &effects);
    ScheduledSegment *oldest_waiting();
    uint64_t generation_ = 0;
    uint64_t segment_high_water_ = 0;
    bool last_dispatch_was_update_ = false;
    TranslationSettingsSnapshot settings_;
    std::vector<ScheduledSegment> segments_;
    std::vector<TranslationJobPtr> queued_;
    std::vector<Running> running_;
};

}
