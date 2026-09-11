#include "caption-pipeline.hpp"
#include "translation-quality.hpp"
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

void CaptionPipeline::record_quality_drops(const std::vector<QualityRetryDrop> &drops, uint64_t now_ms,
                                          CaptionPipelineResult &result)
{
    for (const auto &drop : drops) {
        CaptionPipelineEvent event;
        event.kind = CaptionPipelineEventKind::QualityRetryDropped;
        event.id = drop.id;
        event.request_kind = TranslationRequestKind::QualityRetry;
        event.retry_outcome = drop.outcome;
        event.at_ms = now_ms;
        event.queue_depth = queued_count();
        fill_quality_event(event, scheduler_.find(drop.id.segment));
        result.events.push_back(event);
    }
}

void CaptionPipeline::fill_quality_event(CaptionPipelineEvent &event,
                                         const ScheduledSegment *state) const
{
    if (!state) return;
    event.quality_reason = state->quality_reason;
    event.quality_verdict = state->quality_verdict;
    event.detect_us = state->quality_detect_us;
    event.similarity_computed = state->similarity_computed;
    event.similarity = state->similarity;
}

QualityJudgement CaptionPipeline::inspect_segment(const ScheduledSegment &state,
                                                  std::string_view output) const
{
    const auto &settings = scheduler_.translation_settings();
    QualityInspectInput input;
    input.target_code = settings.target_code;
    input.source = state.segment.source_text;
    input.output = output;
    return inspect_translation_quality(input);
}

void CaptionPipeline::maybe_reserve_quality(const SegmentKey &key, uint64_t now_ms,
                                            CaptionPipelineResult &result)
{
    const auto *state = scheduler_.find(key);
    if (!state || !state->quality_suspected_pending || state->quality_retry_consumed) return;
    if (state->display != CaptionDisplayState::Visible) return;
    auto status = composer_.display_status(key);
    const auto snap = composer_.snapshot();
    const uint64_t publication = (snap.key && *snap.key == key) ? snap.publication : 0;
    const size_t page_index = status ? status->page_index : 0;
    const uint64_t expires = status && status->page_expires_ms ? *status->page_expires_ms : 0;
    auto retry = scheduler_.request_quality_retry(key, context(state->segment), now_ms,
                                                  state->quality_reason, publication, page_index,
                                                  expires);
    scheduler_effects(retry.effects, now_ms, result);
    if (retry.accepted) {
        const auto *queued = scheduler_.find(key);
        if (queued)
            display_events(composer_.expect_job(queued->current_job, now_ms).events, now_ms, result);
    }
}

void CaptionPipeline::scheduler_effects(const SchedulerEffects &effects, uint64_t now_ms,
                                       CaptionPipelineResult &result)
{
    for (const auto &id : effects.cancel)
        if (std::find(result.cancel.begin(), result.cancel.end(), id) == result.cancel.end())
            result.cancel.push_back(id);
    record_quality_drops(effects.quality_drops, now_ms, result);
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
                const auto status = composer_.display_status(display.key);
                const auto snap = composer_.snapshot();
                const uint64_t publication =
                    (snap.key && *snap.key == display.key) ? snap.publication : 0;
                const size_t page_index = status ? status->page_index : display.page_index;
                const uint64_t expires = status && status->page_expires_ms ? *status->page_expires_ms : 0;
                SchedulerEffects page_effects;
                scheduler_.note_display_page(display.key, publication, page_index, expires, now_ms,
                                             page_effects);
                scheduler_effects(page_effects, now_ms, result);
                segmenter_.set_display_state(display.key, CaptionDisplayState::Visible);
                for (auto &logical : logical_) {
                    if (logical.segment.key != display.key) continue;
                    logical.state = CaptionDisplayState::Visible;
                    if (logical.ready_ms) event.display_wait_ms = elapsed(*logical.ready_ms, now_ms);
                }
                maybe_reserve_quality(display.key, now_ms, result);
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
    quality_retry_ = config.quality_retry;
    scheduler_.set_quality_retry(config.quality_retry, now_ms);
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

CaptionPipelineResult CaptionPipeline::set_quality_retry(bool enabled, uint64_t now_ms)
{
    CaptionPipelineResult result;
    if (!generation()) { result.accepted = false; return result; }
    if (quality_retry_ == enabled) return result;
    quality_retry_ = enabled;
    auto changed = scheduler_.set_quality_retry(enabled, now_ms);
    scheduler_effects(changed.effects, now_ms, result);
    CaptionPipelineEvent event;
    event.kind = CaptionPipelineEventKind::QualityRetryChanged;
    event.id.segment.generation = generation();
    event.at_ms = now_ms;
    event.count = enabled ? 1 : 0;
    event.queue_depth = queued_count();
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
        if (result.job->request_kind == TranslationRequestKind::QualityRetry) {
            event.retry_queue_ms = event.queue_wait_ms;
            fill_quality_event(event, scheduler_.find(result.job->id.segment));
        }
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
    const bool quality_retry = completion.id.attempt_id > 1;
    QualityJudgement judgement;
    bool inspected = false;
    std::optional<QualityRetryOutcome> retry_outcome;
    // Scheduler payload validation can turn nominal success into terminal failure.
    if (completed.accepted || completed.reject == SchedulerRejectReason::TranslationLimit ||
        completed.reject == SchedulerRejectReason::InvalidUtf8 ||
        completed.reject == SchedulerRejectReason::EmptyText) {
        auto effective = completion;
        effective.outcome = completed.outcome;
        effective.failure = completed.failure;
        if (effective.outcome != TranslationOutcome::Success) effective.translated_text.clear();
        const auto *state = scheduler_.find(completion.id.segment);
        if (completed.accepted && completed.outcome == TranslationOutcome::Success && state) {
            judgement = inspect_segment(*state, completion.translated_text);
            inspected = true;
            const bool max_tokens = completion.finish_reason == "MAX_TOKENS";
            if (!quality_retry) {
                scheduler_.set_quality_judgement(completion.id.segment, judgement,
                                                 !max_tokens && quality_retry_);
                CaptionPipelineEvent inspect;
                inspect.kind = CaptionPipelineEventKind::QualityInspected;
                inspect.id = completion.id;
                inspect.request_kind = kind(completion.id);
                inspect.at_ms = now_ms;
                inspect.queue_depth = queued_count();
                fill_quality_event(inspect, scheduler_.find(completion.id.segment));
                result.events.push_back(inspect);
            } else {
                const auto snap = composer_.snapshot();
                const auto status = composer_.display_status(completion.id.segment);
                const bool page_current =
                    snap.key && *snap.key == completion.id.segment &&
                    (!state->quality_page_publication ||
                     (snap.publication == state->quality_page_publication &&
                      snap.display.page_index == state->quality_page_index));
                const uint64_t deadline = [&] {
                    uint64_t value = state->quality_expires_ms;
                    if (status && status->page_expires_ms) {
                        if (value == 0) value = *status->page_expires_ms;
                        else value = std::min(value, *status->page_expires_ms);
                    }
                    return value;
                }();
                const bool expired = deadline != 0 && now_ms >= deadline;
                if (!quality_retry_) retry_outcome = QualityRetryOutcome::Disabled;
                else if (!page_current) retry_outcome = QualityRetryOutcome::Superseded;
                else if (expired) retry_outcome = QualityRetryOutcome::Expired;
                else if (judgement.verdict == QualityVerdict::Suspected)
                    retry_outcome = QualityRetryOutcome::StillSuspected;
                else if (judgement.verdict == QualityVerdict::Indeterminate)
                    retry_outcome = QualityRetryOutcome::Indeterminate;
                else retry_outcome = QualityRetryOutcome::Accepted;
                if (*retry_outcome != QualityRetryOutcome::Accepted) {
                    effective.outcome = TranslationOutcome::Failed;
                    effective.translated_text.clear();
                }
            }
        } else if (quality_retry && (completed.accepted ||
                                     completed.reject == SchedulerRejectReason::TranslationLimit ||
                                     completed.reject == SchedulerRejectReason::InvalidUtf8 ||
                                     completed.reject == SchedulerRejectReason::EmptyText)) {
            retry_outcome = QualityRetryOutcome::HttpFailure;
            effective.outcome = TranslationOutcome::Failed;
            effective.translated_text.clear();
        }
        const auto displayed = composer_.complete(effective, now_ms);
        compose_reject = displayed.reject;
        result.accepted = completed.accepted && displayed.accepted;
        if (result.accepted && effective.outcome == TranslationOutcome::Success)
            for (auto &logical : logical_)
                if (logical.segment.key == completion.id.segment)
                    logical.ready_ms = completion.completed_ms;
        display_events(displayed.events, now_ms, result);
        if (!quality_retry && inspected)
            maybe_reserve_quality(completion.id.segment, now_ms, result);
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
    event.retry_outcome = retry_outcome;
    if (quality_retry) event.retry_http_ms = event.http_ms;
    fill_quality_event(event, scheduler_.find(completion.id.segment));
    result.events.push_back(event);
    prune();
    return result;
}

CaptionPipelineResult CaptionPipeline::request_quality_retry(const SegmentKey &key, uint64_t now_ms)
{
    CaptionPipelineResult result;
    const auto *state = scheduler_.find(key);
    if (!state) { result.accepted = false; return result; }
    auto status = composer_.display_status(key);
    const auto snap = composer_.snapshot();
    const uint64_t publication = (snap.key && *snap.key == key) ? snap.publication : 0;
    const size_t page_index = status ? status->page_index : 0;
    const uint64_t expires = status && status->page_expires_ms ? *status->page_expires_ms : 0;
    auto retry = scheduler_.request_quality_retry(key, context(state->segment), now_ms,
                                                  state->quality_reason, publication, page_index,
                                                  expires);
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
    if (rendered.snapshot.key) {
        SchedulerEffects page_effects;
        const auto expires = rendered.snapshot.display.page_expires_ms
                                 ? *rendered.snapshot.display.page_expires_ms
                                 : uint64_t{0};
        scheduler_.note_display_page(*rendered.snapshot.key, rendered.snapshot.publication,
                                     rendered.snapshot.display.page_index, expires, now_ms,
                                     page_effects);
        scheduler_effects(page_effects, now_ms, frame.result);
        maybe_reserve_quality(*rendered.snapshot.key, now_ms, frame.result);
    }
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
