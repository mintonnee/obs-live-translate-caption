#include "caption-composer.hpp"
#include "caption-wrap.hpp"
#include <algorithm>
#include <limits>

namespace lt {
namespace {

constexpr size_t kMaxWindowChunks = 12;

CaptionComposerConfig clamp_config(CaptionComposerConfig cfg)
{
    cfg.max_lines = std::clamp(cfg.max_lines, 1, 6);
    cfg.max_width = std::clamp(cfg.max_width, 10, 120);
    cfg.hold_ms = std::clamp<uint64_t>(cfg.hold_ms, 1000, 30000);
    return cfg;
}

uint64_t deadline(uint64_t start, uint64_t duration)
{
    return start > std::numeric_limits<uint64_t>::max() - duration
        ? std::numeric_limits<uint64_t>::max() : start + duration;
}

std::string normalized_text(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (char ch : text) if (ch != '\r') result.push_back(ch);
    return result;
}

bool same_frame(const CaptionSnapshot &a, const CaptionSnapshot &b)
{
    return a.key == b.key && a.job == b.job && a.text == b.text &&
           a.display.page_index == b.display.page_index &&
           a.display.page_first_visible_ms == b.display.page_first_visible_ms &&
           a.display.page_min_until_ms == b.display.page_min_until_ms &&
           a.display.page_expires_ms == b.display.page_expires_ms;
}

}

CaptionComposer::CaptionComposer(CaptionComposerConfig cfg) : cfg_(clamp_config(cfg))
{
    segments_.reserve(kMaxManaged);
}

CaptionComposer::Segment *CaptionComposer::find(const SegmentKey &key)
{
    return const_cast<Segment *>(static_cast<const CaptionComposer *>(this)->find(key));
}

const CaptionComposer::Segment *CaptionComposer::find(const SegmentKey &key) const
{
    auto it = std::find_if(segments_.begin(), segments_.end(),
                           [&](const auto &s) { return s.source.key == key; });
    return it == segments_.end() ? nullptr : &*it;
}

std::deque<CaptionComposer::Page> CaptionComposer::paginate(
    const std::string &text, size_t begin) const
{
    const auto lines = wrap_text(std::string_view(text).substr(begin), cfg_.max_width);
    std::deque<Page> pages;
    size_t cursor = begin;
    std::vector<std::string> page_lines;
    for (size_t i = 0; i < lines.size(); ++i) {
        // Wrapped lines retain input spans except for CR and edge spaces. Match
        // monotonically to retain raw, codepoint-safe offsets for completed pages.
        if (!lines[i].empty()) {
            const size_t found = text.find(lines[i], cursor);
            if (found != std::string::npos) cursor = found + lines[i].size();
        } else if (cursor < text.size() && text[cursor] == '\n') {
            ++cursor;
        }
        page_lines.push_back(lines[i]);
        if (page_lines.size() == static_cast<size_t>(cfg_.max_lines) || i + 1 == lines.size()) {
            size_t end = cursor;
            if (i + 1 < lines.size() && !lines[i + 1].empty()) {
                const size_t next = text.find(lines[i + 1], cursor);
                if (next != std::string::npos) end = next;
            } else if (i + 1 == lines.size()) {
                end = text.size();
            } else if (!lines[i].empty()) {
                // The next line is explicitly blank. Consume this line's one
                // newline, leaving the next newline to represent that blank.
                while (end < text.size() && text[end] == ' ') ++end;
                if (end < text.size() && text[end] == '\n') ++end;
            }
            pages.push_back({join_lines(page_lines), end});
            page_lines.clear();
            cursor = end;
        }
    }
    return pages;
}

void CaptionComposer::retire_segment(Segment &segment, CaptionRetireReason reason,
                                      uint64_t now_ms, std::vector<CaptionDisplayEvent> &events)
{
    if (segment.status.state == CaptionDisplayState::Retired) return;
    segment.status.state = CaptionDisplayState::Retired;
    segment.status.retire_reason = reason;
    const size_t discarded = segment.pages.size() > 1 ? segment.pages.size() - 1 : 0;
    segment.pages.clear();
    std::string{}.swap(segment.text);
    segment.consumed_bytes = 0;
    events.push_back({CaptionDisplayEventKind::Retired, segment.source.key, reason,
                      segment.status.page_index, discarded, now_ms});
    if (slot_ && slot_->key == segment.source.key) {
        // Only the visible snapshot survives retirement. Version acceptance is
        // already disabled, while the old page retains its reading interval.
        slot_->placeholder = true;
        slot_->retire_after_minimum = reason != CaptionRetireReason::Replaced &&
                                     reason != CaptionRetireReason::ReconcileAmbiguous;
        if (reason == CaptionRetireReason::GenerationChanged) slot_.reset();
    }
}

void CaptionComposer::clear_slot(CaptionRetireReason reason, uint64_t now_ms,
                                 std::vector<CaptionDisplayEvent> &events,
                                 bool preserve_window)
{
    if (!slot_) return;
    const auto key = slot_->key;
    if (auto *owner = find(key)) retire_segment(*owner, reason, now_ms, events);
    for (auto &segment : segments_) {
        if (segment.anchor && *segment.anchor == key)
            retire_segment(segment, reason, now_ms, events);
    }
    slot_.reset();
    if (!preserve_window) window_.clear();
}

std::string CaptionComposer::window_text() const
{
    std::string combined;
    bool first = true;
    for (const auto &chunk : window_) {
        // Translation segment boundaries are not line breaks. Keep page breaks
        // so completed lines cannot flow back into the current reading page.
        if (!first) combined.push_back(chunk.starts_page ? '\n' : ' ');
        combined += chunk.text;
        first = false;
    }
    auto lines = wrap_text(combined, cfg_.max_width);
    const size_t limit = static_cast<size_t>(cfg_.max_lines);
    if (lines.size() > limit) lines.erase(lines.begin(), lines.end() - limit);
    return join_lines(lines);
}

void CaptionComposer::replace_window_chunk(const SegmentKey &old_key,
                                           const SegmentKey &new_key,
                                           const std::string &text,
                                           bool starts_page)
{
    const auto it = std::find_if(window_.rbegin(), window_.rend(), [&](const auto &chunk) {
        return chunk.key == old_key;
    });
    if (it != window_.rend()) {
        it->key = new_key;
        it->text = text;
        it->starts_page = starts_page;
    } else {
        window_.push_back({new_key, text, starts_page});
    }
    while (window_.size() > kMaxWindowChunks) window_.pop_front();
}

void CaptionComposer::expire(uint64_t now_ms, std::vector<CaptionDisplayEvent> &events)
{
    last_now_ms_ = std::max(last_now_ms_, now_ms);
    for (auto &segment : segments_) {
        if (segment.status.state == CaptionDisplayState::Waiting &&
            now_ms >= segment.registered_ms &&
            now_ms - segment.registered_ms >= kStaleBeforeDisplayMs)
            retire_segment(segment, CaptionRetireReason::StaleBeforeDisplay, now_ms, events);
    }
    if (!slot_) return;
    const auto &status = slot_->status;
    const auto *owner = find(slot_->key);
    const bool has_remaining_pages = owner && owner->pages.size() > 1;
    if (has_remaining_pages &&
        now_ms >= deadline(*status.first_visible_ms, kSegmentLifetimeMs) &&
        now_ms >= *status.page_min_until_ms) {
        clear_slot(CaptionRetireReason::DisplayLifetime, now_ms, events);
    } else if (now_ms >= *status.page_expires_ms) {
        // A delayed tick may advance a prepared next page. A completion must
        // still not replace the page whose own deadline has already arrived.
        if (slot_->placeholder || !owner || owner->pages.size() <= 1)
            clear_slot(CaptionRetireReason::Completed, now_ms, events);
    }
}

CaptionComposeResult CaptionComposer::reset_generation(uint64_t generation, uint64_t now_ms)
{
    CaptionComposeResult result;
    if (generation == 0 || generation <= generation_) {
        result.reject = CaptionComposeReject::StaleGeneration;
        return result;
    }
    for (auto &segment : segments_)
        retire_segment(segment, CaptionRetireReason::GenerationChanged, now_ms, result.events);
    slot_.reset();
    window_.clear();
    segments_.clear();
    legacy_sources_.clear();
    generation_ = generation;
    last_now_ms_ = std::max(last_now_ms_, now_ms);
    result.accepted = true;
    return result;
}

CaptionComposeResult CaptionComposer::register_segment(const CaptionSegment &segment,
                                                       uint64_t now_ms)
{
    return register_segments({segment}, now_ms);
}

void CaptionComposer::confirm_visible_final(uint64_t now_ms)
{
    if (slot_->final_confirmed) return;
    slot_->final_confirmed = true;
    slot_->hold_started_ms = now_ms;
    slot_->status.page_expires_ms = deadline(now_ms, cfg_.hold_ms);
    if (auto *segment = find(slot_->key))
        segment->status.page_expires_ms = slot_->status.page_expires_ms;
}

CaptionComposeResult CaptionComposer::confirm_final(const SegmentKey &key, uint64_t now_ms)
{
    CaptionComposeResult result;
    expire(now_ms, result.events);
    if (!key.valid()) result.reject = CaptionComposeReject::InvalidId;
    else if (key.generation != generation_) result.reject = CaptionComposeReject::StaleGeneration;
    else if (!slot_ || slot_->key != key) result.reject = CaptionComposeReject::MissingSegment;
    else if (slot_->final_confirmed) result.reject = CaptionComposeReject::Duplicate;
    else {
        confirm_visible_final(now_ms);
        result.accepted = true;
    }
    return result;
}

CaptionComposeResult CaptionComposer::register_segments(
    const std::vector<CaptionSegment> &input, uint64_t now_ms)
{
    CaptionComposeResult result;
    expire(now_ms, result.events);
    size_t fresh = 0;
    uint64_t new_high_water = high_water_;
    if (input.size() > kMaxManaged) result.reject = CaptionComposeReject::ResourceLimit;
    for (size_t i = 0; result.reject == CaptionComposeReject::None && i < input.size(); ++i) {
        const auto &value = input[i];
        const auto *existing = find(value.key);
        if (!value.key.valid() || value.source_revision == 0)
            result.reject = CaptionComposeReject::InvalidId;
        else if (value.key.generation != generation_)
            result.reject = CaptionComposeReject::StaleGeneration;
        else if (value.source_text.size() > 32 * 1024 || value.replaces.size() > kMaxManaged)
            result.reject = CaptionComposeReject::ResourceLimit;
        else if (existing && existing->status.state == CaptionDisplayState::Retired)
            result.reject = CaptionComposeReject::RetiredSegment;
        else if (existing && value.source_revision < existing->expected.source_revision)
            result.reject = CaptionComposeReject::StaleRevision;
        else if (existing && value.source_revision == existing->expected.source_revision &&
                 value.source_text != existing->source.source_text)
            result.reject = CaptionComposeReject::RevisionConflict;
        else if (!existing && value.key.segment_id <= high_water_)
            result.reject = CaptionComposeReject::ReusedId;
        for (size_t j = 0; j < i; ++j)
            if (input[j].key == value.key) result.reject = CaptionComposeReject::Duplicate;
        if (!existing) ++fresh;
        new_high_water = std::max(new_high_water, value.key.segment_id);
    }
    if (result.reject != CaptionComposeReject::None) return result;
    if (fresh > kMaxManaged - segments_.size()) {
        result.reject = CaptionComposeReject::ResourceLimit;
        for (const auto &value : input)
            result.events.push_back({CaptionDisplayEventKind::Retired, value.key,
                                      CaptionRetireReason::SegmentLimit, 0, 0, now_ms});
        high_water_ = new_high_water;
        return result;
    }
    high_water_ = new_high_water;
    std::vector<SegmentKey> replaceable;
    for (const auto &old : segments_) {
        if (old.status.state != CaptionDisplayState::Retired ||
            (slot_ && (slot_->key == old.source.key ||
                       (old.anchor && *old.anchor == slot_->key))))
            replaceable.push_back(old.source.key);
    }
    for (const auto &value : input) {
        auto *segment = find(value.key);
        if (!segment) {
            segments_.push_back({});
            segment = &segments_.back();
            segment->registered_ms = now_ms;
            segment->source = value;
            segment->expected = {value.key, value.source_revision, 1};
        } else {
            const bool revision_changed = value.source_revision > segment->expected.source_revision;
            const bool confirmed = value.is_final && !segment->source.is_final &&
                slot_ && slot_->key == value.key && !slot_->placeholder;
            const bool finalized = segment->source.is_final || value.is_final;
            segment->source = value;
            segment->source.is_final = finalized;
            if (revision_changed) {
                segment->expected = {value.key, value.source_revision, 1};
                segment->processed = false;
            }
            if (confirmed) confirm_visible_final(now_ms);
        }
        const bool ambiguous = std::any_of(value.replaces.begin(), value.replaces.end(),
                                           [&](const auto &key) {
            return key != value.key &&
                   std::find(replaceable.begin(), replaceable.end(), key) == replaceable.end();
        });
        for (const auto &old_key : value.replaces) {
            if (old_key == value.key) continue;
            auto *old = find(old_key);
            const bool owns_slot = slot_ && slot_->key == old_key;
            if (!old) continue;
            if (owns_slot) {
                if (value.is_final) confirm_visible_final(now_ms);
                segment->anchor = slot_->key;
            }
            else if (old->anchor) segment->anchor = old->anchor;
            bool first_new = true;
            for (const auto &other : input) {
                if (other.order_key < value.order_key &&
                    std::find(other.replaces.begin(), other.replaces.end(), old_key) !=
                        other.replaces.end()) first_new = false;
            }
            if (first_new && old->source.order_key < segment->source.order_key)
                segment->source.order_key = old->source.order_key;
            retire_segment(*old, ambiguous ? CaptionRetireReason::ReconcileAmbiguous
                                          : CaptionRetireReason::Replaced,
                           now_ms, result.events);
        }
        if (ambiguous) {
            retire_segment(*segment, CaptionRetireReason::ReconcileAmbiguous, now_ms, result.events);
            result.reject = CaptionComposeReject::ReconcileAmbiguous;
        }
    }
    enforce_waiting(now_ms, result.events);
    result.accepted = result.reject == CaptionComposeReject::None;
    return result;
}

CaptionComposeResult CaptionComposer::expect_job(const TranslationJobId &id, uint64_t now_ms)
{
    CaptionComposeResult result;
    expire(now_ms, result.events);
    auto *segment = find(id.segment);
    if (!id.valid()) result.reject = CaptionComposeReject::InvalidId;
    else if (id.segment.generation != generation_) result.reject = CaptionComposeReject::StaleGeneration;
    else if (!segment) result.reject = CaptionComposeReject::MissingSegment;
    else if (segment->status.state == CaptionDisplayState::Retired)
        result.reject = CaptionComposeReject::RetiredSegment;
    else if (id.source_revision != segment->expected.source_revision)
        result.reject = CaptionComposeReject::StaleRevision;
    else if (id.attempt_id < segment->expected.attempt_id || id.attempt_id > 2)
        result.reject = CaptionComposeReject::StaleAttempt;
    else {
        if (id.attempt_id > segment->expected.attempt_id) segment->processed = false;
        segment->expected = id;
        result.accepted = true;
    }
    return result;
}

void CaptionComposer::update_visible(Segment &segment, std::string text, uint64_t now_ms,
                                      std::vector<CaptionDisplayEvent> &events)
{
    const size_t consumed = segment.consumed_bytes;
    const bool aligned = !segment.ambiguous_prefix && consumed <= text.size() &&
        text.compare(0, consumed, segment.text, 0, consumed) == 0;
    if (aligned) {
        segment.text = std::move(text);
        segment.pages = paginate(segment.text, consumed);
    } else {
        auto new_pages = paginate(text, 0);
        const size_t selected = std::min(segment.status.page_index, new_pages.size() - 1);
        const size_t discarded = new_pages.size() - 1;
        // Ambiguous prefix: only replace the current ordinal page (clamped to
        // the final page), keep the logical index, and discard all other pages.
        segment.text = new_pages[selected].text;
        segment.pages = {{segment.text, segment.text.size()}};
        segment.consumed_bytes = 0;
        segment.ambiguous_prefix = true;
        events.push_back({CaptionDisplayEventKind::RemainderDiscarded, segment.source.key,
                          CaptionRetireReason::ReconcileAmbiguous, segment.status.page_index,
                          discarded, now_ms});
    }
    if (segment.pages.empty()) {
        // The revision only contains text already displayed. Do not replay it.
        segment.text = slot_->text;
        segment.pages = {{slot_->text, slot_->text.size()}};
        segment.consumed_bytes = 0;
        segment.ambiguous_prefix = true;
    }
    slot_->text = segment.pages.front().text;
    replace_window_chunk(segment.source.key, segment.source.key, slot_->text,
                         segment.status.page_index > 0);
    auto &status = slot_->status;
    status.first_visible_ms = now_ms;
    status.page_first_visible_ms = now_ms;
    status.page_min_until_ms = deadline(now_ms, std::min<uint64_t>(cfg_.hold_ms, 1200));
    status.page_expires_ms = deadline(now_ms, cfg_.hold_ms);
    slot_->hold_started_ms = now_ms;
    segment.status = status;
    slot_->completed_prefix = segment.text.substr(0, segment.consumed_bytes);
    slot_->ambiguous_prefix = segment.ambiguous_prefix;
    slot_->job = segment.expected;
    slot_->replacement_dirty = true;
    events.push_back({CaptionDisplayEventKind::PageReplaced, segment.source.key,
                      CaptionRetireReason::None, segment.status.page_index, 0, now_ms});
}

CaptionComposeResult CaptionComposer::complete(const TranslationCompletion &completion,
                                               uint64_t now_ms)
{
    CaptionComposeResult result;
    expire(now_ms, result.events);
    auto *segment = find(completion.id.segment);
    if (!completion.id.valid()) result.reject = CaptionComposeReject::InvalidId;
    else if (completion.id.segment.generation != generation_)
        result.reject = CaptionComposeReject::StaleGeneration;
    else if (!segment) result.reject = CaptionComposeReject::MissingSegment;
    else if (segment->status.state == CaptionDisplayState::Retired)
        result.reject = CaptionComposeReject::RetiredSegment;
    else if (completion.id.source_revision != segment->expected.source_revision)
        result.reject = CaptionComposeReject::StaleRevision;
    else if (completion.id.attempt_id != segment->expected.attempt_id)
        result.reject = CaptionComposeReject::StaleAttempt;
    else if (segment->processed) result.reject = CaptionComposeReject::Duplicate;
    else if (slot_ && slot_->key == completion.id.segment &&
             now_ms >= *slot_->status.page_expires_ms) {
        segment->processed = true; // A replay cannot correct the next page instead.
        result.reject = CaptionComposeReject::Expired;
    }
    if (result.reject != CaptionComposeReject::None) return result;
    segment->processed = true;
    if (completion.outcome != TranslationOutcome::Success ||
        completion.translated_text.size() > kMaxTranslationBytes) {
        if (segment->pages.empty())
            retire_segment(*segment, CaptionRetireReason::TranslationFailed, now_ms, result.events);
        result.accepted = completion.outcome != TranslationOutcome::Success;
        if (!result.accepted) result.reject = CaptionComposeReject::ResourceLimit;
        return result;
    }
    auto text = normalized_text(completion.translated_text);
    auto pages = paginate(text, 0);
    if (pages.empty() || std::all_of(pages.begin(), pages.end(), [](const auto &page) {
            return page.text.find_first_not_of('\n') == std::string::npos;
        })) {
        if (segment->pages.empty())
            retire_segment(*segment, CaptionRetireReason::TranslationFailed, now_ms, result.events);
        result.reject = CaptionComposeReject::EmptyTranslation;
        return result;
    }
    if (slot_ && slot_->key == completion.id.segment && !slot_->placeholder) {
        update_visible(*segment, std::move(text), now_ms, result.events);
    } else {
        segment->text = std::move(text);
        segment->pages = std::move(pages);
        segment->consumed_bytes = 0;
    }
    segment->translated_job = completion.id;
    result.accepted = true;
    return result;
}

CaptionComposeResult CaptionComposer::retire(const SegmentKey &key, CaptionRetireReason reason,
                                             uint64_t now_ms)
{
    CaptionComposeResult result;
    expire(now_ms, result.events);
    auto *segment = find(key);
    if (!segment) result.reject = CaptionComposeReject::MissingSegment;
    else if (segment->status.state == CaptionDisplayState::Retired)
        result.reject = CaptionComposeReject::RetiredSegment;
    else if (reason != CaptionRetireReason::None) {
        retire_segment(*segment, reason, now_ms, result.events);
        result.accepted = true;
    }
    return result;
}

size_t CaptionComposer::waiting_count() const
{
    return static_cast<size_t>(std::count_if(segments_.begin(), segments_.end(), [](const auto &s) {
        return s.status.state == CaptionDisplayState::Waiting;
    }));
}

void CaptionComposer::enforce_waiting(uint64_t now_ms, std::vector<CaptionDisplayEvent> &events)
{
    while (waiting_count() > kMaxWaiting) {
        Segment *oldest = nullptr;
        for (auto &segment : segments_) {
            if (segment.status.state != CaptionDisplayState::Waiting) continue;
            if (!oldest || segment.registered_ms < oldest->registered_ms ||
                (segment.registered_ms == oldest->registered_ms &&
                 segment.source.order_key < oldest->source.order_key)) oldest = &segment;
        }
        retire_segment(*oldest, CaptionRetireReason::SegmentLimit, now_ms, events);
    }
}

CaptionComposer::Segment *CaptionComposer::next_waiting() const
{
    const Segment *next = nullptr;
    for (const auto &segment : segments_) {
        if (segment.status.state != CaptionDisplayState::Waiting) continue;
        if (!next || segment.source.order_key < next->source.order_key) next = &segment;
    }
    return const_cast<Segment *>(next);
}

CaptionComposer::Segment *CaptionComposer::replacement_head() const
{
    const Segment *head = nullptr;
    if (!slot_) return nullptr;
    for (const auto &segment : segments_) {
        if (segment.status.state != CaptionDisplayState::Waiting || !segment.anchor ||
            *segment.anchor != slot_->key) continue;
        if (!head || segment.source.order_key < head->source.order_key) head = &segment;
    }
    return const_cast<Segment *>(head);
}

void CaptionComposer::enter(Segment &segment, uint64_t now_ms,
                            std::vector<CaptionDisplayEvent> &events, bool inherit)
{
    const auto previous_key = slot_ ? std::optional<SegmentKey>(slot_->key) : std::nullopt;
    auto status = inherit ? slot_->status : segment.status;
    if (inherit && status.page_index > 0) {
        const auto &prefix = slot_->completed_prefix;
        if (!slot_->ambiguous_prefix && !prefix.empty() &&
            segment.text.compare(0, prefix.size(), prefix) == 0 &&
            segment.text.size() > prefix.size()) {
            segment.consumed_bytes = prefix.size();
            segment.pages = paginate(segment.text, segment.consumed_bytes);
            if (segment.pages.empty()) {
                segment.text = slot_->text;
                segment.pages = {{segment.text, segment.text.size()}};
                segment.consumed_bytes = 0;
                segment.ambiguous_prefix = true;
            }
        } else {
            const size_t selected = std::min(status.page_index, segment.pages.size() - 1);
            const size_t discarded = segment.pages.size() - 1;
            segment.text = segment.pages[selected].text;
            segment.pages = {{segment.text, segment.text.size()}};
            segment.consumed_bytes = 0;
            segment.ambiguous_prefix = true;
            events.push_back({CaptionDisplayEventKind::RemainderDiscarded, segment.source.key,
                              CaptionRetireReason::ReconcileAmbiguous, status.page_index,
                              discarded, now_ms});
        }
    }
    if (!inherit) {
        if (!status.first_visible_ms) status.first_visible_ms = now_ms;
        status.page_first_visible_ms = now_ms;
        status.page_min_until_ms = deadline(now_ms, std::min<uint64_t>(cfg_.hold_ms, 1200));
        status.page_expires_ms = deadline(now_ms, cfg_.hold_ms);
    } else {
        status.first_visible_ms = now_ms;
        status.page_first_visible_ms = now_ms;
        status.page_min_until_ms = deadline(now_ms, std::min<uint64_t>(cfg_.hold_ms, 1200));
        status.page_expires_ms = deadline(now_ms, cfg_.hold_ms);
    }
    status.state = CaptionDisplayState::Visible;
    status.retire_reason = CaptionRetireReason::None;
    if (inherit) {
        const auto anchor = slot_->key;
        for (auto &other : segments_)
            if (other.anchor && *other.anchor == anchor) other.anchor.reset();
    }
    segment.status = status;
    Slot next;
    next.key = segment.source.key;
    next.job = segment.translated_job;
    next.status = status;
    next.hold_started_ms = now_ms;
    next.final_confirmed = segment.source.is_final || (inherit && slot_->final_confirmed);
    next.text = segment.pages.front().text;
    next.completed_prefix = segment.text.substr(0, segment.consumed_bytes);
    next.ambiguous_prefix = segment.ambiguous_prefix;
    slot_ = std::move(next);
    if (inherit && previous_key)
        replace_window_chunk(*previous_key, segment.source.key, slot_->text,
                             status.page_index > 0);
    else {
        window_.push_back({segment.source.key, slot_->text, status.page_index > 0});
        while (window_.size() > kMaxWindowChunks) window_.pop_front();
    }
    events.push_back({inherit ? CaptionDisplayEventKind::PageReplaced
                             : CaptionDisplayEventKind::PageEntered,
                      segment.source.key, CaptionRetireReason::None, status.page_index, 0, now_ms});
}

CaptionRenderResult CaptionComposer::tick(uint64_t now_ms)
{
    CaptionRenderResult result;
    expire(now_ms, result.events);
    if (pending_config_) {
        const auto cfg = *pending_config_;
        pending_config_.reset();
        apply_config(cfg, now_ms, result.events);
    }
    bool replaced = false;
    if (slot_ && slot_->placeholder && !slot_->retire_after_minimum) {
        auto *head = replacement_head();
        if (head && !head->pages.empty()) {
            enter(*head, now_ms, result.events, true);
            replaced = true;
        }
    }
    if (slot_ && slot_->replacement_dirty) {
        slot_->replacement_dirty = false;
        replaced = true;
    }
    if (slot_ && !replaced && now_ms >= *slot_->status.page_min_until_ms) {
        auto *current = find(slot_->key);
        auto *next = next_waiting();
        if (slot_->placeholder) {
            if (slot_->retire_after_minimum)
                clear_slot(CaptionRetireReason::Completed, now_ms, result.events);
            else if (next && !next->pages.empty() && !replacement_head())
                clear_slot(CaptionRetireReason::Completed, now_ms, result.events, true);
        } else if (current && current->pages.size() > 1) {
            current->consumed_bytes = current->pages.front().end_byte;
            current->pages.pop_front();
            ++current->status.page_index;
            enter(*current, now_ms, result.events);
            replaced = true;
        } else if (next && !next->pages.empty()) {
            clear_slot(CaptionRetireReason::Completed, now_ms, result.events, true);
        }
    }
    if (!slot_ && !replaced) {
        auto *next = next_waiting();
        if (next && !next->pages.empty()) enter(*next, now_ms, result.events);
    }
    CaptionSnapshot frame;
    frame.publication = published_.publication;
    if (slot_) {
        frame.key = slot_->key;
        frame.job = slot_->job;
        frame.text = window_text();
        frame.display = slot_->status;
    }
    if (!same_frame(frame, published_)) {
        if (frame.publication != std::numeric_limits<uint64_t>::max()) ++frame.publication;
        result.changed = frame.text;
        published_ = frame;
    }
    result.snapshot = published_;
    return result;
}

void CaptionComposer::apply_config(const CaptionComposerConfig &input, uint64_t now_ms,
                                    std::vector<CaptionDisplayEvent> &events)
{
    const auto cfg = clamp_config(input);
    const bool rewrap = cfg.max_width != cfg_.max_width || cfg.max_lines != cfg_.max_lines;
    cfg_ = cfg;
    if (rewrap) {
        for (auto &segment : segments_) {
            if (segment.status.state == CaptionDisplayState::Retired || segment.pages.empty()) continue;
            segment.pages = paginate(segment.text, segment.consumed_bytes);
            if (segment.ambiguous_prefix && segment.pages.size() > 1) {
                events.push_back({CaptionDisplayEventKind::RemainderDiscarded, segment.source.key,
                                  CaptionRetireReason::ReconcileAmbiguous, segment.status.page_index,
                                  segment.pages.size() - 1, now_ms});
                segment.pages.resize(1);
            }
        }
    }
    if (slot_) {
        auto &status = slot_->status;
        status.page_min_until_ms = deadline(*status.page_first_visible_ms,
                                            std::min<uint64_t>(cfg_.hold_ms, 1200));
        status.page_expires_ms = deadline(slot_->hold_started_ms, cfg_.hold_ms);
        if (auto *current = find(slot_->key); current && !slot_->placeholder) {
            current->status = status;
            if (rewrap && !current->pages.empty()) {
                slot_->text = current->pages.front().text;
                replace_window_chunk(slot_->key, slot_->key, slot_->text,
                                     current->status.page_index > 0);
                slot_->replacement_dirty = true;
            }
        } else if (rewrap) {
            auto lines = wrap_text(slot_->text, cfg_.max_width);
            if (lines.size() > static_cast<size_t>(cfg_.max_lines))
                lines.resize(static_cast<size_t>(cfg_.max_lines));
            slot_->text = join_lines(lines);
        }
    }
    expire(now_ms, events);
}

CaptionComposeResult CaptionComposer::set_config(const CaptionComposerConfig &cfg, uint64_t now_ms)
{
    CaptionComposeResult result;
    expire(now_ms, result.events);
    pending_config_.reset();
    apply_config(cfg, now_ms, result.events);
    result.accepted = true;
    return result;
}

void CaptionComposer::set_config(const CaptionComposerConfig &cfg)
{
    pending_config_ = clamp_config(cfg);
}

CaptionComposerConfig CaptionComposer::config() const
{
    return pending_config_ ? *pending_config_ : cfg_;
}

std::optional<CaptionDisplayStatus> CaptionComposer::display_status(const SegmentKey &key) const
{
    const auto *segment = find(key);
    return segment ? std::optional<CaptionDisplayStatus>(segment->status) : std::nullopt;
}

bool CaptionComposer::forget_retired(const SegmentKey &key)
{
    if (slot_ && slot_->key == key) return false;
    const auto it = std::find_if(segments_.begin(), segments_.end(), [&](const auto &s) {
        return s.source.key == key && s.status.state == CaptionDisplayState::Retired;
    });
    if (it == segments_.end()) return false;
    segments_.erase(it);
    return true;
}

void CaptionComposer::push_final(uint64_t seq, const std::string &source_text, uint64_t now_ms)
{
    if (generation_ == 0) reset_generation(1, now_ms);
    if (seq <= high_water_) return;
    CaptionSegment segment{{generation_, seq, seq}, {seq, 0}, 1, source_text, true, {}, now_ms};
    if (!register_segment(segment, now_ms).accepted) return;
    legacy_sources_.push_back(source_text);
    while (legacy_sources_.size() > 16) legacy_sources_.pop_front();
}

void CaptionComposer::on_translated(uint64_t seq, const std::string &text, uint64_t now_ms)
{
    complete({{{generation_, seq, seq}, 1, 1}, TranslationOutcome::Success,
              TranslationFailure::None, text, now_ms, now_ms}, now_ms);
}

void CaptionComposer::on_failed(uint64_t seq, const std::string &reason, uint64_t now_ms)
{
    (void)reason;
    complete({{{generation_, seq, seq}, 1, 1}, TranslationOutcome::Failed,
              TranslationFailure::Network, {}, now_ms, now_ms}, now_ms);
}

std::optional<std::string> CaptionComposer::render(uint64_t now_ms) { return tick(now_ms).changed; }
std::vector<CaptionTruncation> CaptionComposer::take_truncations() { return {}; }

std::vector<std::string> CaptionComposer::context(size_t max) const
{
    const size_t count = std::min(max, legacy_sources_.size());
    return {legacy_sources_.end() - static_cast<ptrdiff_t>(count), legacy_sources_.end()};
}

size_t CaptionComposer::pending_count() const
{
    return static_cast<size_t>(std::count_if(segments_.begin(), segments_.end(), [](const auto &s) {
        return s.status.state != CaptionDisplayState::Retired && !s.processed;
    }));
}

void CaptionComposer::clear()
{
    if (generation_ != std::numeric_limits<uint64_t>::max()) {
        reset_generation(generation_ + 1, last_now_ms_);
    } else {
        slot_.reset();
        window_.clear();
        segments_.clear();
        legacy_sources_.clear();
    }
}

}
