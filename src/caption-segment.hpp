#pragma once
#include "translation-quality-types.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Spec 005 §4.1. Value contracts only; callers serialize state transitions.
namespace lt {

// Zero is invalid. Allocators must not wrap or reuse IDs, including after reset.
struct SegmentKey {
    uint64_t generation = 0;
    uint64_t utterance_id = 0;
    uint64_t segment_id = 0;

    bool valid() const { return generation != 0 && utterance_id != 0 && segment_id != 0; }
    bool operator==(const SegmentKey &other) const
    {
        return std::tie(generation, utterance_id, segment_id) ==
               std::tie(other.generation, other.utterance_id, other.segment_id);
    }
    bool operator!=(const SegmentKey &other) const { return !(*this == other); }
};

struct SegmentKeyHash {
    size_t operator()(const SegmentKey &key) const noexcept
    {
        size_t value = std::hash<uint64_t>{}(key.generation);
        value ^= std::hash<uint64_t>{}(key.utterance_id) + size_t{0x9e3779b9} +
                 (value << 6) + (value >> 2);
        return value ^ (std::hash<uint64_t>{}(key.segment_id) + size_t{0x9e3779b9} +
                        (value << 6) + (value >> 2));
    }
};

// Position is assigned by reconciliation, never inferred from segment_id.
// Zero-based positions may be reassigned together for an affected utterance.
struct SegmentOrderKey {
    uint64_t utterance_order = 0;
    uint64_t position = 0;

    bool operator==(const SegmentOrderKey &other) const
    {
        return std::tie(utterance_order, position) ==
               std::tie(other.utterance_order, other.position);
    }
    bool operator!=(const SegmentOrderKey &other) const { return !(*this == other); }
    bool operator<(const SegmentOrderKey &other) const
    {
        return std::tie(utterance_order, position) <
               std::tie(other.utterance_order, other.position);
    }
};

enum class TranslationRequestKind { Initial, SourceUpdate, QualityRetry };
enum class CaptionDisplayState { Waiting, Visible, Retired };
enum class CaptionRetireReason {
    None,
    Completed,
    Replaced,
    GenerationChanged,
    QueueLimit,
    SegmentLimit,
    SourceLimit,
    StaleBeforeDisplay,
    DisplayLifetime,
    ReconcileAmbiguous,
    TranslationFailed
};

struct TranslationJobId {
    SegmentKey segment;
    uint64_t source_revision = 0; // First source revision is 1.
    uint64_t attempt_id = 0;      // First request is 1; quality retry is 2.

    bool valid() const { return segment.valid() && source_revision != 0 && attempt_id != 0; }
    bool operator==(const TranslationJobId &other) const
    {
        return segment == other.segment && source_revision == other.source_revision &&
               attempt_id == other.attempt_id;
    }
    bool operator!=(const TranslationJobId &other) const { return !(*this == other); }
};

struct TranslationJobIdHash {
    size_t operator()(const TranslationJobId &id) const noexcept
    {
        size_t value = SegmentKeyHash{}(id.segment);
        value ^= std::hash<uint64_t>{}(id.source_revision) + size_t{0x9e3779b9} +
                 (value << 6) + (value >> 2);
        return value ^ (std::hash<uint64_t>{}(id.attempt_id) + size_t{0x9e3779b9} +
                        (value << 6) + (value >> 2));
    }
};

struct CaptionSegment {
    SegmentKey key;
    SegmentOrderKey order_key;
    uint64_t source_revision = 0;
    std::string source_text;
    bool is_final = false;
    std::vector<SegmentKey> replaces;
    uint64_t first_seen_ms = 0; // Monotonic text observation time, not speech onset.
};

struct CaptionDisplayStatus {
    CaptionDisplayState state = CaptionDisplayState::Waiting;
    size_t page_index = 0;
    std::optional<uint64_t> first_visible_ms;
    std::optional<uint64_t> page_first_visible_ms;
    std::optional<uint64_t> page_min_until_ms;
    std::optional<uint64_t> page_expires_ms;
    CaptionRetireReason retire_reason = CaptionRetireReason::None;
};

struct TranslationSettingsSnapshot {
    std::string target_code;
    std::string target_name;
    std::string model;
    std::vector<std::string> glossary; // STT names; never force source spelling.
};

// Construct with all fields. Queues share an immutable value, never live settings.
// Credentials belong to the generation's transport, not to loggable metadata.
struct TranslationJob {
    const TranslationJobId id;
    const SegmentOrderKey order_key;
    const TranslationRequestKind request_kind;
    const std::string source_text;
    const std::vector<std::string> context;
    const TranslationSettingsSnapshot settings;
    const uint64_t first_seen_ms;
    const uint64_t first_registered_ms; // Preserved across source updates and retries.
    const uint64_t queued_ms;
    const QualitySuspectReason quality_reason;

    TranslationJob(TranslationJobId job_id, SegmentOrderKey order,
                   TranslationRequestKind kind, std::string source,
                   std::vector<std::string> previous_context,
                   TranslationSettingsSnapshot settings_snapshot, uint64_t observed_ms,
                   uint64_t registered_ms, uint64_t enqueued_ms,
                   QualitySuspectReason quality = QualitySuspectReason::None)
        : id(job_id), order_key(order), request_kind(kind), source_text(std::move(source)),
          context(std::move(previous_context)), settings(std::move(settings_snapshot)),
          first_seen_ms(observed_ms), first_registered_ms(registered_ms), queued_ms(enqueued_ms),
          quality_reason(quality)
    {
    }
};

using TranslationJobPtr = std::shared_ptr<const TranslationJob>;

enum class TranslationOutcome { Success, Failed, Cancelled };
enum class TranslationFailure { None, Network, Http, Auth, Parse, Empty, ResponseLimit };

// Do not log this payload; log its id, enum outcome and monotonic times only.
struct TranslationCompletion {
    TranslationJobId id;
    TranslationOutcome outcome = TranslationOutcome::Failed;
    TranslationFailure failure = TranslationFailure::None;
    std::string translated_text;
    uint64_t started_ms = 0;
    uint64_t completed_ms = 0;
    std::string finish_reason; // candidates[0].finishReason; do not quality-retry MAX_TOKENS
};

// Identity matching is necessary but not sufficient: under the same state lock,
// also reject retired/replaced/missing segments and already consumed results.
inline bool matches_current_job(const TranslationJobId &result, const TranslationJobId &current)
{
    return result.valid() && current.valid() && result == current;
}

}
