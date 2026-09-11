#include "caption-pipeline.hpp"
#include <algorithm>

namespace lt {
namespace {
uint64_t elapsed(uint64_t since, uint64_t now) { return now >= since ? now - since : 0; }

TranslationRequestKind kind(const TranslationJobId &id)
{
    return id.attempt_id > 1 ? TranslationRequestKind::QualityRetry
        : id.source_revision > 1 ? TranslationRequestKind::SourceUpdate : TranslationRequestKind::Initial;
}
}

bool CaptionPublicationGate::claim(uint64_t publication)
{
    if (publication <= last_) return false;
    last_ = publication;
    return true;
}

void CaptionPipeline::record_retire(const SegmentKey &key, CaptionRetireReason reason,
                                    uint64_t now_ms, CaptionPipelineResult &result)
{
    segmenter_.set_display_state(key, CaptionDisplayState::Retired);
    for (auto &logical : logical_) {
        if (logical.segment.key == key) {
            logical.state = CaptionDisplayState::Retired;
            if (logical.reason == CaptionRetireReason::None) logical.reason = reason;
        }
    }
    const bool recorded = std::any_of(result.events.begin(), result.events.end(), [&](const auto &e) {
        return e.kind == CaptionPipelineEventKind::Retired && e.id.segment == key;
    });
    if (!recorded) {
        CaptionPipelineEvent event;
        event.kind = CaptionPipelineEventKind::Retired;
        event.id.segment = key;
        event.retire_reason = reason;
        event.at_ms = now_ms;
        event.queue_depth = queued_count();
        result.events.push_back(event);
    }
}

void CaptionPipeline::scheduler_effects(const SchedulerEffects &effects, uint64_t now_ms,
                                       CaptionPipelineResult &result)
{
    for (const auto &id : effects.cancel)
        if (std::find(result.cancel.begin(), result.cancel.end(), id) == result.cancel.end())
            result.cancel.push_back(id);
    for (const auto &retired : effects.retired) {
        record_retire(retired.key, retired.reason, now_ms, result);
        display_events(composer_.retire(retired.key, retired.reason, now_ms).events, now_ms, result);
    }
}

void CaptionPipeline::display_events(const std::vector<CaptionDisplayEvent> &events,
                                     uint64_t now_ms, CaptionPipelineResult &result)
{
    for (const auto &display : events) {
        if (display.kind == CaptionDisplayEventKind::Retired) {
            record_retire(display.key, display.reason, now_ms, result);
            scheduler_effects(scheduler_.retire(display.key, display.reason).effects, now_ms, result);
            continue;
        }
        CaptionPipelineEvent event;
        event.id.segment = display.key;
        event.at_ms = now_ms;
        event.retire_reason = display.reason;
        event.count = display.discarded_pages;
        if (display.kind == CaptionDisplayEventKind::RemainderDiscarded) {
            event.kind = CaptionPipelineEventKind::RemainderDiscarded;
        } else {
            event.kind = display.kind == CaptionDisplayEventKind::PageEntered
                ? CaptionPipelineEventKind::PageEntered : CaptionPipelineEventKind::PageReplaced;
            const auto *state = scheduler_.find(display.key);
            if (state && state->display != CaptionDisplayState::Retired) {
                event.id = state->current_job;
                event.request_kind = kind(event.id);
                event.first_seen_to_display_ms = elapsed(state->segment.first_seen_ms, now_ms);
                scheduler_effects(scheduler_.mark_visible(display.key, now_ms).effects, now_ms, result);
                segmenter_.set_display_state(display.key, CaptionDisplayState::Visible);
                for (auto &logical : logical_) {
                    if (logical.segment.key != display.key) continue;
                    logical.state = CaptionDisplayState::Visible;
                    if (logical.ready_ms) event.display_wait_ms = elapsed(*logical.ready_ms, now_ms);
                }
            }
        }
        event.queue_depth = queued_count();
        result.events.push_back(event);
    }
}

CaptionPipelineResult CaptionPipeline::reset(uint64_t generation,
                                            const CaptionPipelineConfig &config, uint64_t now_ms)
{
    CaptionPipelineResult result;
    auto reset = scheduler_.reset_generation(generation, config.translation);
    result.accepted = reset.accepted;
    if (!reset.accepted) return result;
    // Reset the composer directly before forwarding scheduler events, so old
    // visible slots do not transiently retain a previous generation's display.
    auto display = composer_.reset_generation(generation, now_ms);
    scheduler_effects(reset.effects, now_ms, result);
    display_events(display.events, now_ms, result);
    segmenter_.reset(generation);
    segmenter_.set_incremental(config.incremental);
    incremental_ = config.incremental;
    display_events(composer_.set_config(config.display, now_ms).events, now_ms, result);
    logical_.clear();
    current_utterance_ = 0;
    finalized_ = false;
    CaptionPipelineEvent event;
    event.kind = CaptionPipelineEventKind::GenerationChanged;
    event.id.segment.generation = generation;
    event.at_ms = now_ms;
    result.events.push_back(event);
    return result;
}

CaptionPipelineResult CaptionPipeline::set_display_config(const CaptionComposerConfig &config,
                                                         uint64_t now_ms)
{
    CaptionPipelineResult result;
    display_events(composer_.set_config(config, now_ms).events, now_ms, result);
    return result;
}

CaptionPipelineResult CaptionPipeline::set_incremental(bool enabled, uint64_t now_ms)
{
    CaptionPipelineResult result;
    if (!generation()) { result.accepted = false; return result; }
    if (incremental_ == enabled) return result;
    incremental_ = enabled;
    segmenter_.set_incremental(enabled);
    if (!enabled && !finalized_) {
        // Keep S1 identities and the visible page. A later identical final may
        // confirm those identities but must not recreate the retired jobs.
        std::vector<SegmentKey> retire;
        for (const auto &logical : logical_)
            if (logical.segment.key.utterance_id == current_utterance_ &&
                !logical.segment.is_final && logical.state == CaptionDisplayState::Waiting)
                retire.push_back(logical.segment.key);
        for (const auto &key : retire) {
            // The final-only policy replaces these reservations; it does not
            // change generation or retire already displayed provisional text.
            scheduler_effects(scheduler_.retire(key, CaptionRetireReason::Replaced).effects,
                              now_ms, result);
            display_events(composer_.retire(key, CaptionRetireReason::Replaced, now_ms).events,
                           now_ms, result);
        }
    }
    CaptionPipelineEvent event;
    event.kind = CaptionPipelineEventKind::IncrementalChanged;
    event.id.segment.generation = generation();
    event.at_ms = now_ms;
    event.count = enabled ? 1 : 0;
    result.events.push_back(event);
    return result;
}

std::vector<std::string> CaptionPipeline::context(const CaptionSegment &segment) const
{
    std::vector<const CaptionSegment *> earlier;
    for (const auto &logical : logical_) {
        if (logical.segment.order_key < segment.order_key &&
            (logical.state != CaptionDisplayState::Retired ||
             logical.reason == CaptionRetireReason::Completed)) earlier.push_back(&logical.segment);
    }
    std::sort(earlier.begin(), earlier.end(), [](const auto *a, const auto *b) {
        return a->order_key < b->order_key;
    });
    std::vector<std::string> result;
    const size_t first = earlier.size() > 3 ? earlier.size() - 3 : 0;
    for (size_t i = first; i < earlier.size(); ++i) result.push_back(earlier[i]->source_text);
    return result;
}

void CaptionPipeline::prune()
{
    std::vector<SegmentKey> recent;
    for (auto it = logical_.rbegin(); it != logical_.rend() && recent.size() < 3; ++it)
        if (it->reason == CaptionRetireReason::Completed) recent.push_back(it->segment.key);
    logical_.erase(std::remove_if(logical_.begin(), logical_.end(), [&](const auto &logical) {
        if (logical.state != CaptionDisplayState::Retired ||
            (!finalized_ && logical.segment.key.utterance_id == current_utterance_) ||
            std::find(recent.begin(), recent.end(), logical.segment.key) != recent.end()) return false;
        const auto status = composer_.display_status(logical.segment.key);
        if (status && !composer_.forget_retired(logical.segment.key)) return false;
        scheduler_.forget_retired(logical.segment.key);
        return true;
    }), logical_.end());
}

void CaptionPipeline::apply(const SegmenterResult &changes, uint64_t now_ms,
                            CaptionPipelineResult &result)
{
    if (!segmenter_.segments().empty())
        current_utterance_ = segmenter_.segments().front().key.utterance_id;
    else current_utterance_ = 0;
    // Final metadata includes retained S1 tombstones. Remember them before
    // prune forgets their scheduler/composer records, avoiding ID reuse and an
    // atomic registration rejection that would also discard a valid new tail.
    std::vector<SegmentKey> retired_keys;
    for (const auto &logical : logical_)
        if (logical.state == CaptionDisplayState::Retired)
            retired_keys.push_back(logical.segment.key);
    prune();
    scheduler_effects(scheduler_.tick(now_ms), now_ms, result);
    std::vector<CaptionSegment> upserts;
    for (const auto &segment : changes.upserts) {
        if (std::find(retired_keys.begin(), retired_keys.end(), segment.key) != retired_keys.end())
            continue;
        const auto *existing = scheduler_.find(segment.key);
        if (!existing || existing->display != CaptionDisplayState::Retired) {
            upserts.push_back(segment);
            // replaces is a one-time topology mutation, not a revision property.
            if (existing) upserts.back().replaces.clear();
        }
    }
    // Register replacement groups before forwarding their Replaced retirements.
    const auto registration = composer_.register_segments(upserts, now_ms);
    display_events(registration.events, now_ms, result);
    if (!registration.accepted) {
        CaptionPipelineEvent rejected;
        rejected.kind = CaptionPipelineEventKind::Rejected;
        rejected.composer_reject = registration.reject;
        rejected.at_ms = now_ms;
        rejected.count = upserts.size();
        result.events.push_back(rejected);
    }
    for (const auto &segment : upserts) {
        const auto status = composer_.display_status(segment.key);
        if (!status) continue;
        auto existing = std::find_if(logical_.begin(), logical_.end(), [&](const auto &logical) {
            return logical.segment.key == segment.key;
        });
        if (existing != logical_.end()) existing->segment = segment;
        else if (logical_.size() < TranslationScheduler::kMaxManaged)
            logical_.push_back({segment, status->state, status->retire_reason});
    }
    std::sort(logical_.begin(), logical_.end(), [](const auto &a, const auto &b) {
        return a.segment.order_key < b.segment.order_key;
    });
    std::sort(upserts.begin(), upserts.end(), [](const auto &a, const auto &b) {
        return a.key.segment_id < b.key.segment_id;
    });
    for (const auto &segment : upserts) {
        const auto display = composer_.display_status(segment.key);
        if (!display || display->state == CaptionDisplayState::Retired) continue;
        auto scheduled = scheduler_.register_segment(segment, context(segment), now_ms);
        scheduler_effects(scheduled.effects, now_ms, result);
        if (!scheduled.accepted && scheduled.reject != SchedulerRejectReason::Duplicate) {
            display_events(composer_.retire(segment.key, CaptionRetireReason::SegmentLimit,
                                             now_ms).events, now_ms, result);
        }
        CaptionPipelineEvent event;
        event.kind = scheduled.accepted ? CaptionPipelineEventKind::Registered
                                       : CaptionPipelineEventKind::Rejected;
        event.id = {segment.key, segment.source_revision, 1};
        event.request_kind = kind(event.id);
        event.scheduler_reject = scheduled.reject;
        event.at_ms = now_ms;
        event.queue_depth = queued_count();
        result.events.push_back(event);
    }
    for (const auto &retirement : changes.retired) {
        scheduler_effects(scheduler_.retire(retirement.key, retirement.reason).effects, now_ms, result);
        display_events(composer_.retire(retirement.key, retirement.reason, now_ms).events, now_ms, result);
        record_retire(retirement.key, retirement.reason, now_ms, result);
    }
    const std::pair<CaptionPipelineEventKind, size_t> counts[] = {
        {CaptionPipelineEventKind::SourceLimit, changes.source_limit},
        {CaptionPipelineEventKind::SegmentLimit, changes.segment_limit},
        {CaptionPipelineEventKind::ReconcileAmbiguous, changes.reconcile_ambiguous}};
    for (const auto &entry : counts) {
        if (!entry.second) continue;
        CaptionPipelineEvent event;
        event.kind = entry.first;
        event.count = entry.second;
        event.at_ms = now_ms;
        result.events.push_back(event);
    }
    prune();
}

CaptionPipelineResult CaptionPipeline::interim(std::string_view text, uint64_t now_ms)
{
    CaptionPipelineResult result;
    if (!generation()) { result.accepted = false; return result; }
    finalized_ = false;
    apply(segmenter_.interim(text, now_ms), now_ms, result);
    return result;
}

CaptionPipelineResult CaptionPipeline::final(std::string_view text, uint64_t now_ms)
{
    CaptionPipelineResult result;
    if (!generation()) { result.accepted = false; return result; }
    const auto visible = composer_.snapshot().key;
    if (!finalized_ && visible && visible->utterance_id == current_utterance_)
        display_events(composer_.confirm_final(*visible, now_ms).events, now_ms, result);
    finalized_ = true;
    apply(segmenter_.final(text, now_ms), now_ms, result);
    return result;
}

CaptionPipelineDispatch CaptionPipeline::dispatch(uint64_t now_ms)
{
    CaptionPipelineDispatch result;
    auto dispatch = scheduler_.dispatch(now_ms);
    scheduler_effects(dispatch.effects, now_ms, result.result);
    result.job = dispatch.job;
    if (result.job) {
        display_events(composer_.expect_job(result.job->id, now_ms).events, now_ms, result.result);
        CaptionPipelineEvent event;
        event.kind = CaptionPipelineEventKind::Dispatched;
        event.id = result.job->id;
        event.request_kind = result.job->request_kind;
        event.at_ms = now_ms;
        event.segment_wait_ms = elapsed(result.job->first_seen_ms, result.job->first_registered_ms);
        event.queue_wait_ms = elapsed(result.job->queued_ms, now_ms);
        event.queue_depth = queued_count();
        result.result.events.push_back(event);
    }
    return result;
}

CaptionPipelineResult CaptionPipeline::complete(const TranslationCompletion &completion,
                                               uint64_t now_ms)
{
    CaptionPipelineResult result;
    const auto completed = scheduler_.complete(completion, now_ms);
    result.accepted = completed.accepted;
    CaptionComposeReject compose_reject = CaptionComposeReject::None;
    // Scheduler payload validation can turn nominal success into terminal failure.
    if (completed.accepted || completed.reject == SchedulerRejectReason::TranslationLimit ||
        completed.reject == SchedulerRejectReason::InvalidUtf8 ||
        completed.reject == SchedulerRejectReason::EmptyText) {
        auto effective = completion;
        effective.outcome = completed.outcome;
        effective.failure = completed.failure;
        if (effective.outcome != TranslationOutcome::Success) effective.translated_text.clear();
        const auto displayed = composer_.complete(effective, now_ms);
        compose_reject = displayed.reject;
        result.accepted = completed.accepted && displayed.accepted;
        if (result.accepted && effective.outcome == TranslationOutcome::Success)
            for (auto &logical : logical_)
                if (logical.segment.key == completion.id.segment)
                    logical.ready_ms = completion.completed_ms;
        display_events(displayed.events, now_ms, result);
        if (completion.failure == TranslationFailure::Auth && completed.accepted && displayed.accepted)
            result.auth_error = true;
    }
    scheduler_effects(completed.effects, now_ms, result);
    CaptionPipelineEvent event;
    event.kind = result.accepted ? CaptionPipelineEventKind::Completed
                                   : CaptionPipelineEventKind::Rejected;
    event.id = completion.id;
    event.request_kind = kind(completion.id);
    event.scheduler_reject = completed.reject;
    event.composer_reject = compose_reject;
    event.failure = completed.failure;
    event.at_ms = now_ms;
    event.http_ms = elapsed(completion.started_ms, completion.completed_ms);
    event.queue_depth = queued_count();
    result.events.push_back(event);
    prune();
    return result;
}

CaptionPipelineResult CaptionPipeline::request_quality_retry(const SegmentKey &key, uint64_t now_ms)
{
    CaptionPipelineResult result;
    const auto *state = scheduler_.find(key);
    if (!state) { result.accepted = false; return result; }
    auto retry = scheduler_.request_quality_retry(key, context(state->segment), now_ms);
    result.accepted = retry.accepted;
    scheduler_effects(retry.effects, now_ms, result);
    if (retry.accepted)
        display_events(composer_.expect_job(scheduler_.find(key)->current_job, now_ms).events,
                       now_ms, result);
    return result;
}

CaptionPipelineFrame CaptionPipeline::tick(uint64_t now_ms)
{
    CaptionPipelineFrame frame;
    if (generation()) apply(segmenter_.tick(now_ms), now_ms, frame.result);
    scheduler_effects(scheduler_.tick(now_ms), now_ms, frame.result);
    auto rendered = composer_.tick(now_ms);
    display_events(rendered.events, now_ms, frame.result);
    frame.changed = rendered.changed;
    frame.snapshot = rendered.snapshot;
    prune();
    return frame;
}

size_t CaptionPipeline::managed_count() const
{
    return std::max({scheduler_.managed_count(), composer_.managed_count(), logical_.size(),
                     segmenter_.managed_count()});
}

std::vector<CaptionSegment> CaptionPipeline::segments() const
{
    std::vector<CaptionSegment> result;
    for (const auto &logical : logical_)
        if (logical.state != CaptionDisplayState::Retired) result.push_back(logical.segment);
    return result;
}

}
