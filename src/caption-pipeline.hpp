#pragma once
#include "caption-composer.hpp"
#include "caption-segmenter.hpp"
#include "translation-scheduler.hpp"

namespace lt {

struct CaptionPipelineConfig {
    TranslationSettingsSnapshot translation;
    CaptionComposerConfig display;
    bool incremental = true;
    bool quality_retry = true;
};

enum class CaptionPipelineEventKind {
    Registered, Dispatched, Completed, Rejected, Retired, PageEntered, PageReplaced,
    RemainderDiscarded, SourceLimit, SegmentLimit, ReconcileAmbiguous, GenerationChanged,
    IncrementalChanged, QualityInspected, QualityRetryDropped, QualityRetryChanged
};

// Metadata only: safe to log without source, translation or transport credentials.
struct CaptionPipelineEvent {
    CaptionPipelineEventKind kind = CaptionPipelineEventKind::Registered;
    TranslationJobId id;
    TranslationRequestKind request_kind = TranslationRequestKind::Initial;
    CaptionRetireReason retire_reason = CaptionRetireReason::None;
    SchedulerRejectReason scheduler_reject = SchedulerRejectReason::None;
    CaptionComposeReject composer_reject = CaptionComposeReject::None;
    TranslationFailure failure = TranslationFailure::None;
    uint64_t at_ms = 0;
    uint64_t segment_wait_ms = 0;
    uint64_t queue_wait_ms = 0;
    uint64_t http_ms = 0;
    uint64_t display_wait_ms = 0;
    uint64_t first_seen_to_display_ms = 0;
    size_t count = 1;
    size_t queue_depth = 0;
    QualitySuspectReason quality_reason = QualitySuspectReason::None;
    QualityVerdict quality_verdict = QualityVerdict::Indeterminate;
    std::optional<QualityRetryOutcome> retry_outcome;
    uint64_t detect_us = 0;
    uint64_t retry_queue_ms = 0;
    uint64_t retry_http_ms = 0;
    bool similarity_computed = false;
    double similarity = 0.0;
};

struct CaptionPipelineResult {
    bool accepted = true;
    bool auth_error = false; // Only an accepted current-generation auth failure.
    std::vector<TranslationJobId> cancel;
    std::vector<CaptionPipelineEvent> events;
};

struct CaptionPipelineDispatch {
    TranslationJobPtr job;
    CaptionPipelineResult result;
};

struct CaptionPipelineFrame {
    std::optional<std::string> changed;
    CaptionSnapshot snapshot;
    CaptionPipelineResult result;
};

// Thread-unsafe reducer, no I/O. Keep the instance across stop/reconnect: the
// scheduler must retain physically occupied slots until completion confirmation.
class CaptionPipeline {
public:
    CaptionPipelineResult reset(uint64_t generation, const CaptionPipelineConfig &config,
                                uint64_t now_ms);
    CaptionPipelineResult set_display_config(const CaptionComposerConfig &config, uint64_t now_ms);
    // No generation change: disabling retires only unseen current provisional work.
    CaptionPipelineResult set_incremental(bool enabled, uint64_t now_ms);
    // No generation change: cancels queued quality jobs and ignores in-flight retry display.
    CaptionPipelineResult set_quality_retry(bool enabled, uint64_t now_ms);
    CaptionPipelineResult interim(std::string_view text, uint64_t now_ms);
    CaptionPipelineResult final(std::string_view text, uint64_t now_ms);
    CaptionPipelineDispatch dispatch(uint64_t now_ms);
    CaptionPipelineResult complete(const TranslationCompletion &completion, uint64_t now_ms);
    CaptionPipelineResult request_quality_retry(const SegmentKey &key, uint64_t now_ms);
    CaptionPipelineFrame tick(uint64_t now_ms);
    uint64_t generation() const { return scheduler_.generation(); }
    size_t queued_count() const { return scheduler_.queued_count(); }
    size_t in_flight_count() const { return scheduler_.in_flight_count(); }
    size_t managed_count() const;
    size_t waiting_count() const { return scheduler_.waiting_count(); }
    size_t source_snapshot_bytes() const { return segmenter_.snapshot_bytes(); }
    const ScheduledSegment *find(const SegmentKey &key) const { return scheduler_.find(key); }
    CaptionSnapshot snapshot() const { return composer_.snapshot(); }
    std::vector<CaptionSegment> segments() const;

private:
    struct LogicalSegment {
        CaptionSegment segment;
        CaptionDisplayState state = CaptionDisplayState::Waiting;
        CaptionRetireReason reason = CaptionRetireReason::None;
        std::optional<uint64_t> ready_ms;
    };
    void apply(const SegmenterResult &changes, uint64_t now_ms, CaptionPipelineResult &result);
    void scheduler_effects(const SchedulerEffects &effects, uint64_t now_ms,
                          CaptionPipelineResult &result);
    void display_events(const std::vector<CaptionDisplayEvent> &events, uint64_t now_ms,
                        CaptionPipelineResult &result);
    void record_retire(const SegmentKey &key, CaptionRetireReason reason, uint64_t now_ms,
                       CaptionPipelineResult &result);
    void record_quality_drops(const std::vector<QualityRetryDrop> &drops, uint64_t now_ms,
                              CaptionPipelineResult &result);
    void maybe_reserve_quality(const SegmentKey &key, uint64_t now_ms, CaptionPipelineResult &result);
    QualityJudgement inspect_segment(const ScheduledSegment &state, std::string_view output) const;
    void fill_quality_event(CaptionPipelineEvent &event, const ScheduledSegment *state) const;
    std::vector<std::string> context(const CaptionSegment &segment) const;
    void prune();
    CaptionSegmenter segmenter_;
    TranslationScheduler scheduler_;
    CaptionComposer composer_;
    std::vector<LogicalSegment> logical_;
    uint64_t current_utterance_ = 0;
    bool finalized_ = false;
    bool incremental_ = true;
    bool quality_retry_ = true;
};

// A sink adapter holds its publication mutex across claim + actual sink call.
// Stale snapshots are rejected even if producers deliver them in reverse order.
class CaptionPublicationGate {
public:
    bool claim(uint64_t publication);
    uint64_t last_publication() const { return last_; }
private:
    uint64_t last_ = 0;
};

}
