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
    DisplayConflict,
    NotVisible,
    InsufficientLifetime,
    QualityDisabled,
    QualityQueueFull
};

struct SchedulerRetirement {
    SegmentKey key;
    CaptionRetireReason reason = CaptionRetireReason::None;
};

struct QualityRetryDrop {
    TranslationJobId id;
    QualityRetryOutcome outcome = QualityRetryOutcome::Expired;
};

// Per-transition effects, never a retained/unbounded event queue. Apply retirements
// to the composer under the caller's state lock; issue cancellations outside it.
struct SchedulerEffects {
    std::vector<SchedulerRetirement> retired;
    std::vector<TranslationJobId> cancel;
    std::vector<QualityRetryDrop> quality_drops;
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
    QualityVerdict quality_verdict = QualityVerdict::Indeterminate;
    QualitySuspectReason quality_reason = QualitySuspectReason::None;
    uint64_t quality_detect_us = 0;
    bool similarity_computed = false;
    double similarity = 0.0;
    bool quality_retry_consumed = false;
    bool quality_suspected_pending = false;
    uint64_t quality_page_publication = 0;
    size_t quality_page_index = 0;
    uint64_t quality_expires_ms = 0;
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
    // One attempt 2 per revision. Callers must pass a currently visible segment.
    SchedulerResult request_quality_retry(const SegmentKey &key,
                                          const std::vector<std::string> &context,
                                          uint64_t now_ms,
                                          QualitySuspectReason reason = QualitySuspectReason::None,
                                          uint64_t page_publication = 0, size_t page_index = 0,
                                          uint64_t expires_ms = 0);
    SchedulerDispatch dispatch(uint64_t now_ms);
    SchedulerCompletionResult complete(const TranslationCompletion &completion, uint64_t now_ms);
    SchedulerResult mark_visible(const SegmentKey &key, uint64_t now_ms);
    SchedulerResult retire(const SegmentKey &key, CaptionRetireReason reason);
    SchedulerEffects tick(uint64_t now_ms);
    // No generation change: drop queued retries and ignore later in-flight display.
    SchedulerResult set_quality_retry(bool enabled, uint64_t now_ms);
    void set_quality_judgement(const SegmentKey &key, const QualityJudgement &judgement,
                               bool eligible_for_retry);
    void note_display_page(const SegmentKey &key, uint64_t publication, size_t page_index,
                           uint64_t expires_ms, uint64_t now_ms, SchedulerEffects &effects);
    // Only after caller has finished final reconciliation for this metadata.
    bool forget_retired(const SegmentKey &key);

    // Borrowed pointer invalidated by the next mutation. Do not log text fields.
    const ScheduledSegment *find(const SegmentKey &key) const;
    size_t queued_count() const { return queued_.size(); }
    size_t in_flight_count() const { return running_.size(); }
    size_t managed_count() const { return segments_.size(); }
    size_t waiting_count() const;
    size_t quality_queued_count() const;
    size_t quality_in_flight_count() const;
    bool quality_retry_enabled() const { return quality_retry_enabled_; }
    uint64_t generation() const { return generation_; }
    const TranslationSettingsSnapshot &translation_settings() const { return settings_; }

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
                 SchedulerEffects &effects, QualitySuspectReason quality = QualitySuspectReason::None);
    void enforce_limits(SchedulerEffects &effects);
    void reset_quality_state(ScheduledSegment &segment);
    void drop_quality_job(const TranslationJobPtr &job, QualityRetryOutcome outcome,
                          SchedulerEffects &effects);
    void drop_quality_for_segment(const SegmentKey &key, QualityRetryOutcome outcome,
                                  SchedulerEffects &effects);
    void expire_quality_retries(uint64_t now_ms, SchedulerEffects &effects);
    bool quality_remaining_ok(const ScheduledSegment &segment, uint64_t now_ms) const;
    bool is_quality_job(const TranslationJobPtr &job) const;
    bool is_quality_id(const TranslationJobId &id) const;
    ScheduledSegment *oldest_waiting();
    uint64_t generation_ = 0;
    uint64_t segment_high_water_ = 0;
    bool last_dispatch_was_update_ = false;
    bool quality_retry_enabled_ = true;
    TranslationSettingsSnapshot settings_;
    std::vector<ScheduledSegment> segments_;
    std::vector<TranslationJobPtr> queued_;
    std::vector<Running> running_;
};

}
