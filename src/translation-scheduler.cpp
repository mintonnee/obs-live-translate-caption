#include "translation-scheduler.hpp"

#include <algorithm>

namespace lt {
namespace {

bool valid_utf8(const std::string &text)
{
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) continue;
        size_t count = 0;
        uint32_t value = 0;
        uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            count = 1;
            value = lead & 0x1f;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            count = 2;
            value = lead & 0x0f;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            count = 3;
            value = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (count > text.size() - i) return false;
        while (count-- != 0) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}

std::vector<std::string> bounded_context(const std::vector<std::string> &context)
{
    // Discard whole oldest entries; never slice a UTF-8 sequence or sentence.
    std::vector<std::string> result;
    size_t bytes = 0;
    for (size_t i = context.size();
         i > 0 && result.size() < TranslationScheduler::kMaxContextSegments; --i) {
        const auto &entry = context[i - 1];
        if (entry.size() > TranslationScheduler::kMaxContextBytes - bytes) break;
        if (!valid_utf8(entry)) break;
        bytes += entry.size();
        result.push_back(entry);
    }
    std::reverse(result.begin(), result.end());
    return result;
}

}

const ScheduledSegment *TranslationScheduler::find(const SegmentKey &key) const
{
    const auto it = std::find_if(segments_.begin(), segments_.end(),
                                 [&](const auto &s) { return s.segment.key == key; });
    return it == segments_.end() ? nullptr : &*it;
}

ScheduledSegment *TranslationScheduler::find_mutable(const SegmentKey &key)
{
    return const_cast<ScheduledSegment *>(static_cast<const TranslationScheduler *>(this)->find(key));
}

size_t TranslationScheduler::waiting_count() const
{
    return static_cast<size_t>(std::count_if(segments_.begin(), segments_.end(), [](const auto &s) {
        return s.display == CaptionDisplayState::Waiting;
    }));
}

void TranslationScheduler::cancel_running(const SegmentKey &key, SchedulerEffects &effects)
{
    for (auto &run : running_) {
        if (run.id.segment == key && !run.cancel_requested) {
            run.cancel_requested = true;
            effects.cancel.push_back(run.id);
        }
    }
}

void TranslationScheduler::retire_segment(ScheduledSegment &segment, CaptionRetireReason reason,
                                          SchedulerEffects &effects)
{
    if (segment.display == CaptionDisplayState::Retired) return;
    segment.display = CaptionDisplayState::Retired;
    segment.retire_reason = reason;
    std::string{}.swap(segment.translated_text);
    const auto key = segment.segment.key;
    queued_.erase(std::remove_if(queued_.begin(), queued_.end(),
                                 [&](const auto &job) { return job->id.segment == key; }),
                  queued_.end());
    cancel_running(key, effects);
    effects.retired.push_back({key, reason});
}

SchedulerResult TranslationScheduler::reset_generation(
    uint64_t generation, const TranslationSettingsSnapshot &settings)
{
    SchedulerResult result;
    if (generation == 0 || generation <= generation_) {
        result.reject = SchedulerRejectReason::StaleGeneration;
        return result;
    }
    for (auto &segment : segments_)
        retire_segment(segment, CaptionRetireReason::GenerationChanged, result.effects);
    segments_.clear();
    queued_.clear();
    generation_ = generation;
    settings_ = settings;
    last_dispatch_was_update_ = false;
    result.accepted = true;
    return result;
}

SchedulerEffects TranslationScheduler::tick(uint64_t now_ms)
{
    SchedulerEffects effects;
    for (auto &segment : segments_) {
        if (segment.display == CaptionDisplayState::Waiting &&
            now_ms >= segment.first_registered_ms &&
            now_ms - segment.first_registered_ms >= kStaleBeforeDisplayMs)
            retire_segment(segment, CaptionRetireReason::StaleBeforeDisplay, effects);
    }
    return effects;
}

ScheduledSegment *TranslationScheduler::oldest_waiting()
{
    ScheduledSegment *oldest = nullptr;
    for (auto &segment : segments_) {
        if (segment.display != CaptionDisplayState::Waiting) continue;
        if (!oldest || segment.first_registered_ms < oldest->first_registered_ms ||
            (segment.first_registered_ms == oldest->first_registered_ms &&
             segment.segment.order_key < oldest->segment.order_key))
            oldest = &segment;
    }
    return oldest;
}

void TranslationScheduler::enforce_limits(SchedulerEffects &effects)
{
    while (waiting_count() > kMaxWaiting || queued_.size() > kMaxQueued) {
        auto *oldest = oldest_waiting();
        if (!oldest) break; // Only one logical segment may be visible at a time.
        retire_segment(*oldest, queued_.size() > kMaxQueued ? CaptionRetireReason::QueueLimit
                                                         : CaptionRetireReason::SegmentLimit,
                       effects);
    }
}

void TranslationScheduler::enqueue(ScheduledSegment &segment, TranslationRequestKind kind,
                                   const std::vector<std::string> &context, uint64_t now_ms,
                                   SchedulerEffects &effects)
{
    const auto key = segment.segment.key;
    queued_.erase(std::remove_if(queued_.begin(), queued_.end(),
                                 [&](const auto &job) { return job->id.segment == key; }),
                  queued_.end());
    cancel_running(key, effects);
    queued_.push_back(std::make_shared<const TranslationJob>(
        segment.current_job, segment.segment.order_key, kind, segment.segment.source_text,
        bounded_context(context), settings_, segment.segment.first_seen_ms,
        segment.first_registered_ms, now_ms));
    segment.result_processed = false;
    enforce_limits(effects);
}

SchedulerResult TranslationScheduler::register_segment(
    const CaptionSegment &input, const std::vector<std::string> &context, uint64_t now_ms)
{
    SchedulerResult result;
    result.effects = tick(now_ms);
    if (!input.key.valid() || input.source_revision == 0) {
        result.reject = SchedulerRejectReason::InvalidId;
        return result;
    }
    if (input.key.generation != generation_) {
        result.reject = SchedulerRejectReason::StaleGeneration;
        return result;
    }
    auto *segment = find_mutable(input.key);
    if (segment && segment->display == CaptionDisplayState::Retired) {
        result.reject = SchedulerRejectReason::RetiredSegment;
        return result;
    }
    if (!segment && input.key.segment_id <= segment_high_water_) {
        result.reject = SchedulerRejectReason::ReusedId;
        return result;
    }
    if (segment && input.source_revision < segment->segment.source_revision) {
        result.reject = SchedulerRejectReason::StaleRevision;
        return result;
    }
    if (input.source_text.empty() || input.source_text.size() > kMaxSourceBytes ||
        input.replaces.size() > kMaxManaged ||
        !valid_utf8(input.source_text)) {
        result.reject = input.source_text.empty() ? SchedulerRejectReason::EmptyText
            : input.source_text.size() > kMaxSourceBytes || input.replaces.size() > kMaxManaged
                ? SchedulerRejectReason::SourceLimit : SchedulerRejectReason::InvalidUtf8;
        if (segment) retire_segment(*segment, CaptionRetireReason::SourceLimit, result.effects);
        else {
            segment_high_water_ = input.key.segment_id;
            result.effects.retired.push_back({input.key, CaptionRetireReason::SourceLimit});
        }
        return result;
    }
    if (segment && input.source_revision == segment->segment.source_revision) {
        if (input.source_text != segment->segment.source_text) {
            result.reject = SchedulerRejectReason::RevisionConflict;
            return result;
        }
        segment->segment.is_final = segment->segment.is_final || input.is_final;
        segment->segment.order_key = input.order_key;
        for (auto &job : queued_) {
            if (job->id.segment != input.key || job->order_key == input.order_key) continue;
            job = std::make_shared<const TranslationJob>(
                job->id, input.order_key, job->request_kind, job->source_text, job->context,
                job->settings, job->first_seen_ms, job->first_registered_ms, job->queued_ms);
        }
        result.reject = SchedulerRejectReason::Duplicate;
        return result;
    }
    const bool fresh = segment == nullptr;
    if (fresh) {
        segment_high_water_ = input.key.segment_id;
        if (segments_.size() >= kMaxManaged) {
            // Preserve current-utterance tombstones for final reconciliation.
            // Caller must stop incremental splitting and use bounded final input.
            result.reject = SchedulerRejectReason::ManagedLimit;
            result.effects.retired.push_back({input.key, CaptionRetireReason::SegmentLimit});
            return result;
        }
        segments_.push_back({});
        segment = &segments_.back();
        segment->first_registered_ms = now_ms;
    }
    const uint64_t first_seen = fresh ? input.first_seen_ms : segment->segment.first_seen_ms;
    segment->segment = input;
    segment->segment.first_seen_ms = first_seen;
    segment->current_job = {input.key, input.source_revision, 1};
    for (const auto &replaced : input.replaces) {
        if (replaced == input.key) continue;
        auto *old = find_mutable(replaced);
        if (old) retire_segment(*old, CaptionRetireReason::Replaced, result.effects);
    }
    enqueue(*segment, fresh ? TranslationRequestKind::Initial : TranslationRequestKind::SourceUpdate,
            context, now_ms, result.effects);
    result.accepted = segment->display != CaptionDisplayState::Retired;
    if (!result.accepted) result.reject = SchedulerRejectReason::RetiredSegment;
    return result;
}

SchedulerResult TranslationScheduler::request_quality_retry(
    const SegmentKey &key, const std::vector<std::string> &context, uint64_t now_ms)
{
    SchedulerResult result;
    result.effects = tick(now_ms);
    auto *segment = find_mutable(key);
    if (key.generation != generation_) result.reject = SchedulerRejectReason::StaleGeneration;
    else if (!segment) result.reject = SchedulerRejectReason::MissingSegment;
    else if (segment->display == CaptionDisplayState::Retired)
        result.reject = SchedulerRejectReason::RetiredSegment;
    else if (segment->current_job.attempt_id != 1) result.reject = SchedulerRejectReason::Duplicate;
    else {
        segment->current_job.attempt_id = 2;
        enqueue(*segment, TranslationRequestKind::QualityRetry, context, now_ms, result.effects);
        result.accepted = segment->display != CaptionDisplayState::Retired;
        if (!result.accepted) result.reject = SchedulerRejectReason::RetiredSegment;
    }
    return result;
}

SchedulerDispatch TranslationScheduler::dispatch(uint64_t now_ms)
{
    SchedulerDispatch result;
    result.effects = tick(now_ms);
    if (running_.size() >= kMaxInFlight || queued_.empty()) return result;
    const bool prefer_initial = last_dispatch_was_update_ &&
        std::any_of(queued_.begin(), queued_.end(), [](const auto &job) {
            return job->request_kind == TranslationRequestKind::Initial;
        });
    auto rank = [&](const TranslationJobPtr &job) {
        const auto *segment = find(job->id.segment);
        if (prefer_initial && job->request_kind == TranslationRequestKind::Initial)
            return 0;
        if (!prefer_initial && segment->display == CaptionDisplayState::Visible &&
            job->request_kind == TranslationRequestKind::SourceUpdate) return 0;
        return job->request_kind == TranslationRequestKind::QualityRetry ? 2 : 1;
    };
    auto next = std::min_element(queued_.begin(), queued_.end(), [&](const auto &a, const auto &b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return find(a->id.segment)->segment.order_key < find(b->id.segment)->segment.order_key;
    });
    result.job = *next;
    queued_.erase(next);
    running_.push_back({result.job->id, false});
    last_dispatch_was_update_ = result.job->request_kind == TranslationRequestKind::SourceUpdate;
    return result;
}

SchedulerCompletionResult TranslationScheduler::complete(
    const TranslationCompletion &completion, uint64_t now_ms)
{
    SchedulerCompletionResult result;
    result.effects = tick(now_ms);
    result.outcome = completion.outcome;
    result.failure = completion.failure;
    const auto run = std::find_if(running_.begin(), running_.end(),
                                   [&](const auto &r) { return r.id == completion.id; });
    const bool dispatched = run != running_.end();
    if (dispatched) running_.erase(run); // Even obsolete callbacks release their real slot.
    auto *segment = find_mutable(completion.id.segment);
    if (!completion.id.valid()) result.reject = SchedulerRejectReason::InvalidId;
    else if (completion.id.segment.generation != generation_)
        result.reject = SchedulerRejectReason::StaleGeneration;
    else if (!segment) result.reject = SchedulerRejectReason::MissingSegment;
    else if (segment->display == CaptionDisplayState::Retired)
        result.reject = SchedulerRejectReason::RetiredSegment;
    else if (completion.id.source_revision != segment->current_job.source_revision)
        result.reject = SchedulerRejectReason::StaleRevision;
    else if (completion.id.attempt_id != segment->current_job.attempt_id)
        result.reject = SchedulerRejectReason::StaleAttempt;
    else if (segment->result_processed) result.reject = SchedulerRejectReason::Duplicate;
    else if (!dispatched) result.reject = SchedulerRejectReason::NotDispatched;
    else {
        segment->result_processed = true;
        if (completion.outcome == TranslationOutcome::Success) {
            if (completion.translated_text.size() > kMaxTranslationBytes) {
                result.reject = SchedulerRejectReason::TranslationLimit;
                result.failure = TranslationFailure::ResponseLimit;
            } else if (!valid_utf8(completion.translated_text)) {
                result.reject = SchedulerRejectReason::InvalidUtf8;
                result.failure = TranslationFailure::Parse;
            } else if (completion.translated_text.empty()) {
                result.reject = SchedulerRejectReason::EmptyText;
                result.failure = TranslationFailure::Empty;
            }
            if (result.reject != SchedulerRejectReason::None)
                result.outcome = TranslationOutcome::Failed;
        }
        result.accepted = result.reject == SchedulerRejectReason::None;
        if (result.outcome == TranslationOutcome::Success)
            segment->translated_text = completion.translated_text;
        else if (segment->translated_text.empty())
            retire_segment(*segment, CaptionRetireReason::TranslationFailed, result.effects);
    }
    return result;
}

SchedulerResult TranslationScheduler::mark_visible(const SegmentKey &key, uint64_t now_ms)
{
    SchedulerResult result;
    result.effects = tick(now_ms);
    auto *segment = find_mutable(key);
    if (!segment) result.reject = SchedulerRejectReason::MissingSegment;
    else if (segment->display == CaptionDisplayState::Retired)
        result.reject = SchedulerRejectReason::RetiredSegment;
    else if (segment->translated_text.empty()) result.reject = SchedulerRejectReason::EmptyText;
    else if (std::any_of(segments_.begin(), segments_.end(), [&](const auto &other) {
                 return other.segment.key != key && other.display == CaptionDisplayState::Visible;
             })) result.reject = SchedulerRejectReason::DisplayConflict;
    else {
        segment->display = CaptionDisplayState::Visible;
        result.accepted = true;
    }
    return result;
}

SchedulerResult TranslationScheduler::retire(const SegmentKey &key, CaptionRetireReason reason)
{
    SchedulerResult result;
    auto *segment = find_mutable(key);
    if (!segment) result.reject = SchedulerRejectReason::MissingSegment;
    else if (segment->display == CaptionDisplayState::Retired)
        result.reject = SchedulerRejectReason::RetiredSegment;
    else if (reason != CaptionRetireReason::None) {
        retire_segment(*segment, reason, result.effects);
        result.accepted = true;
    }
    return result;
}

bool TranslationScheduler::forget_retired(const SegmentKey &key)
{
    const auto it = std::find_if(segments_.begin(), segments_.end(), [&](const auto &segment) {
        return segment.segment.key == key && segment.display == CaptionDisplayState::Retired;
    });
    if (it == segments_.end()) return false;
    segments_.erase(it);
    return true;
}

}
