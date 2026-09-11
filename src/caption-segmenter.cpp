#include "caption-segmenter.hpp"
#include "caption-wrap.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace lt {
namespace {

struct Point {
    uint32_t value;
    size_t begin;
    size_t end;
};

std::vector<Point> points(std::string_view text)
{
    std::vector<Point> out;
    for (size_t i = 0; i < text.size();) {
        const size_t begin = i;
        const auto head = static_cast<unsigned char>(text[i]);
        size_t length = head < 0x80 ? 1 : head >= 0xc2 && head <= 0xdf ? 2 :
                        head >= 0xe0 && head <= 0xef ? 3 :
                        head >= 0xf0 && head <= 0xf4 ? 4 : 1;
        uint32_t cp = head & (length == 1 ? 0xff : (1u << (7 - length)) - 1);
        bool valid = i + length <= text.size();
        for (size_t j = 1; valid && j < length; ++j) {
            const auto c = static_cast<unsigned char>(text[i + j]);
            valid = (c & 0xc0) == 0x80;
            cp = (cp << 6) | (c & 0x3f);
        }
        valid = valid && !(length == 2 && cp < 0x80) &&
                !(length == 3 && cp < 0x800) && !(length == 4 && cp < 0x10000) &&
                !(cp >= 0xd800 && cp <= 0xdfff) && cp <= 0x10ffff;
        if (!valid) { length = 1; cp = 0xfffd; }
        i += length;
        out.push_back({cp, begin, i});
    }
    return out;
}

bool space(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0x3000;
}

bool sentence(const std::vector<Point> &p, size_t i)
{
    const auto cp = p[i].value;
    if (cp == '.' && i && i + 1 < p.size() && p[i - 1].value >= '0' &&
        p[i - 1].value <= '9' && p[i + 1].value >= '0' && p[i + 1].value <= '9')
        return false;
    return cp == '.' || cp == '!' || cp == '?' || cp == 0x3002 ||
           cp == 0xff01 || cp == 0xff1f;
}

int priority(const std::vector<Point> &p, size_t i)
{
    if (sentence(p, i)) return 3;
    const auto cp = p[i].value;
    if (cp == ',' || cp == ';' || cp == '\n' || cp == 0x3001 ||
        cp == 0xff0c || cp == 0xff1b) return 2;
    return space(cp) ? 1 : 0;
}

size_t common_points(const std::vector<Point> &a, const std::vector<Point> &b)
{
    size_t n = 0;
    while (n < a.size() && n < b.size() && a[n].value == b[n].value) ++n;
    return n;
}

uint64_t allocate(uint64_t &counter)
{
    if (counter == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("caption identifier exhausted");
    return ++counter;
}

std::string bounded_tail(std::string_view text)
{
    size_t begin = text.size() > CaptionSegmenter::kMaxSourceBytes ?
                   text.size() - CaptionSegmenter::kMaxSourceBytes : 0;
    while (begin < text.size() &&
           (static_cast<unsigned char>(text[begin]) & 0xc0) == 0x80) ++begin;
    return normalize_caption_source(text.substr(begin));
}

// Match whole code points and whitespace, never a historical byte count.
size_t match_prefix(std::string_view text, const std::vector<CaptionSegment> &old,
                    size_t &matched)
{
    const auto p = points(text);
    size_t pos = 0;
    matched = 0;
    for (const auto &segment : old) {
        while (pos < p.size() && space(p[pos].value)) ++pos;
        const auto source = points(segment.source_text);
        if (pos + source.size() > p.size()) break;
        size_t n = 0;
        while (n < source.size() && source[n].value == p[pos + n].value) ++n;
        if (n != source.size()) break;
        const auto word = [](uint32_t cp) {
            return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
                   (cp >= '0' && cp <= '9');
        };
        if (n && pos + n < p.size() && word(source.back().value) &&
            word(p[pos + n].value) && display_width(segment.source_text) < 80) break;
        pos += n;
        ++matched;
    }
    while (pos < p.size() && space(p[pos].value)) ++pos;
    return pos == p.size() ? text.size() : p[pos].begin;
}

}

std::string normalize_caption_source(std::string_view text)
{
    std::string out;
    char separator = 0;
    for (const auto &p : points(text)) {
        if (space(p.value)) {
            if (p.value == '\n') separator = '\n';
            else if (!separator) separator = ' ';
        } else {
            if (!out.empty() && separator) out += separator;
            separator = 0;
            out.append(text.substr(p.begin, p.end - p.begin));
        }
    }
    return out;
}

std::vector<std::string> split_caption_source(std::string_view text)
{
    const auto normalized = normalize_caption_source(text);
    const auto p = points(normalized);
    std::vector<std::string> out;
    for (size_t begin = 0; begin < p.size();) {
        while (begin < p.size() && space(p[begin].value)) ++begin;
        if (begin == p.size()) break;
        size_t end = begin;
        size_t candidates[4] = {};
        int width = 0;
        while (end < p.size()) {
            const int w = display_width(std::string_view(normalized).substr(
                p[end].begin, p[end].end - p[end].begin));
            if (width + w > 120) break;
            width += w;
            const int rank = priority(p, end);
            if (width >= 12 || rank == 3) candidates[rank] = end + 1;
            ++end;
        }
        size_t cut = end;
        if (end < p.size() || width >= 80) {
            for (int rank = 3; rank >= 0; --rank) {
                if (candidates[rank]) { cut = candidates[rank]; break; }
            }
        }
        // A complete short sentence is a valid boundary even below the target.
        if (width < 80 && candidates[3]) cut = candidates[3];
        size_t trimmed = cut;
        while (trimmed > begin && space(p[trimmed - 1].value)) --trimmed;
        if (trimmed > begin) out.emplace_back(normalized.substr(
            p[begin].begin, p[trimmed - 1].end - p[begin].begin));
        begin = cut;
    }
    return out;
}

CaptionSegmenter::CaptionSegmenter(uint64_t generation) : generation_(generation)
{
    if (!generation) throw std::invalid_argument("caption generation must be nonzero");
}

void CaptionSegmenter::reset(uint64_t generation)
{
    if (!generation) throw std::invalid_argument("caption generation must be nonzero");
    generation_ = generation;
    utterance_ = 0;
    closed_ = false;
    stopped_ = false;
    tombstones_ = 0;
    segments_.clear();
    states_.clear();
    snapshots_.clear();
    unchanged_since_.clear();
}

void CaptionSegmenter::begin_utterance(uint64_t now_ms)
{
    utterance_ = allocate(next_utterance_);
    first_seen_ms_ = now_ms;
    closed_ = false;
    stopped_ = false;
    tombstones_ = 0;
    snapshots_.clear();
    unchanged_since_.clear();
    segments_.clear();
    states_.clear();
}

void CaptionSegmenter::set_incremental(bool enabled) { incremental_ = enabled; }

void CaptionSegmenter::set_display_state(const SegmentKey &key, CaptionDisplayState state)
{
    for (size_t i = 0; i < segments_.size(); ++i)
        if (segments_[i].key == key) states_[i] = state;
}

size_t CaptionSegmenter::snapshot_bytes() const
{
    size_t bytes = 0;
    for (const auto &snapshot : snapshots_) bytes += snapshot.size();
    return bytes;
}

SegmenterResult CaptionSegmenter::interim(std::string_view text, uint64_t now_ms)
{
    if (text.empty()) return {};
    if (!utterance_ || closed_) begin_utterance(now_ms);
    if (text.size() > kMaxSourceBytes) {
        stopped_ = true;
        snapshots_.clear();
        unchanged_since_.clear();
        SegmenterResult out;
        out.source_limit = 1;
        return out;
    }
    const auto normalized = normalize_caption_source(text);
    if (normalized.empty() || stopped_) return {};
    if (!snapshots_.empty() && snapshots_.back() == normalized) return {};
    const auto current = points(normalized);
    const size_t same = snapshots_.empty() ? 0 :
                        common_points(points(snapshots_.back()), current);
    unchanged_since_.resize(same);
    unchanged_since_.resize(current.size(), now_ms);
    snapshots_.push_back(normalized);
    if (snapshots_.size() > kMaxSnapshots) snapshots_.pop_front();
    return evaluate(now_ms);
}

SegmenterResult CaptionSegmenter::tick(uint64_t now_ms) { return evaluate(now_ms); }

SegmenterResult CaptionSegmenter::evaluate(uint64_t now_ms)
{
    if (!incremental_ || stopped_ || closed_ || snapshots_.size() < kMaxSnapshots) return {};
    const auto p = points(snapshots_.back());
    size_t stable = p.size();
    for (const auto &snapshot : snapshots_)
        stable = std::min(stable, common_points(p, points(snapshot)));
    size_t aged = 0;
    while (aged < stable && now_ms >= unchanged_since_[aged] &&
           now_ms - unchanged_since_[aged] >= 800) ++aged;
    stable = aged;
    if (!stable) return {};
    size_t end = stable;
    while (end && space(p[end - 1].value)) --end;
    if (!end) return {};
    if (!sentence(p, end - 1)) {
        size_t word = end;
        while (word && !space(p[word - 1].value)) --word;
        end = word ? word : end > 4 ? end - 4 : 0;
        while (end && space(p[end - 1].value)) --end;
    }
    if (!end) return {};
    const auto candidate = snapshots_.back().substr(0, p[end - 1].end);
    size_t matched = 0;
    const auto tail_byte = match_prefix(candidate, segments_, matched);
    const auto tail = std::string_view(candidate).substr(matched == segments_.size() ?
                                                       tail_byte : 0);
    const int width = display_width(tail);
    if (tail.empty()) return {};
    const bool terminal = sentence(p, end - 1);
    size_t tail_point = 0;
    while (tail_point < p.size() && p[tail_point].begin < tail_byte) ++tail_point;
    const uint64_t tail_seen = tail_point < unchanged_since_.size() ?
                               unchanged_since_[tail_point] : now_ms;
    if (!terminal && (width < 12 || (width < 80 &&
        (now_ms < tail_seen || now_ms - tail_seen < 2000)))) return {};
    return reconcile(candidate, false, now_ms);
}

SegmenterResult CaptionSegmenter::final(std::string_view text, uint64_t now_ms)
{
    const bool limited = text.size() > kMaxSourceBytes;
    const auto normalized = limited ? bounded_tail(text) : normalize_caption_source(text);
    if (!utterance_ || closed_) begin_utterance(now_ms);
    auto out = reconcile(normalized, true, now_ms);
    out.source_limit += limited ? 1 : 0;
    closed_ = true;
    snapshots_.clear();
    unchanged_since_.clear();
    return out;
}

SegmenterResult CaptionSegmenter::reconcile(const std::string &text, bool is_final,
                                           uint64_t now_ms)
{
    SegmenterResult out;
    size_t prefix = 0;
    const auto tail_begin = match_prefix(text, segments_, prefix);
    std::vector<std::string> parts;
    for (size_t i = 0; i < prefix; ++i) parts.push_back(segments_[i].source_text);
    const auto tail = split_caption_source(std::string_view(text).substr(tail_begin));
    parts.insert(parts.end(), tail.begin(), tail.end());
    size_t suffix = 0;
    while (suffix + prefix < segments_.size() && suffix + prefix < parts.size() &&
           segments_[segments_.size() - suffix - 1].source_text ==
               parts[parts.size() - suffix - 1]) ++suffix;
    const size_t old_count = segments_.size() - prefix - suffix;
    const size_t new_count = parts.size() - prefix - suffix;
    bool retired_affected = false;
    for (size_t i = prefix; i < prefix + old_count; ++i)
        retired_affected |= states_[i] == CaptionDisplayState::Retired;
    if (retired_affected) {
        stopped_ = true;
        ++out.reconcile_ambiguous;
        for (size_t i = prefix; i < prefix + old_count; ++i) {
            out.retired.push_back({segments_[i].key, CaptionRetireReason::ReconcileAmbiguous});
            states_[i] = CaptionDisplayState::Retired;
        }
        return out;
    }
    bool revision = old_count == 1 && new_count == 1;
    if (revision) {
        const auto before = points(segments_[prefix].source_text);
        const auto after = points(parts[prefix]);
        size_t same = common_points(before, after);
        size_t back = 0;
        while (back < before.size() && back < after.size() &&
               before[before.size() - back - 1].value == after[after.size() - back - 1].value)
            ++back;
        revision = same + back >= 2;
    }
    const size_t added = revision ? 0 : new_count;
    if (managed_count() + added > kMaxSegments) {
        stopped_ = true;
        ++out.segment_limit;
        if (!is_final) return out;
        if (std::find(states_.begin(), states_.end(), CaptionDisplayState::Retired) !=
            states_.end()) {
            ++out.reconcile_ambiguous;
            return out;
        }
        for (const auto &segment : segments_)
            out.retired.push_back({segment.key, CaptionRetireReason::SegmentLimit});
        segments_.clear();
        states_.clear();
        tombstones_ = 0;
        auto bounded = split_caption_source(text);
        if (bounded.size() > kMaxSegments)
            bounded.erase(bounded.begin(), bounded.end() - kMaxSegments);
        for (const auto &part : bounded) {
            CaptionSegment segment{{generation_, utterance_, allocate(next_segment_)},
                                   {utterance_, segments_.size()}, 1, part, true, {}, now_ms};
            segments_.push_back(segment);
            states_.push_back(CaptionDisplayState::Waiting);
            out.upserts.push_back(std::move(segment));
        }
        return out;
    }
    std::vector<CaptionSegment> next;
    std::vector<CaptionDisplayState> next_states;
    std::vector<SegmentKey> replaces;
    if (!revision) {
        for (size_t i = prefix; i < prefix + old_count; ++i) {
            replaces.push_back(segments_[i].key);
            out.retired.push_back({segments_[i].key, CaptionRetireReason::Replaced});
        }
        tombstones_ += old_count;
    }
    const auto text_points = points(text);
    const size_t observed_prefix = snapshots_.empty() ? 0 :
        common_points(text_points, points(snapshots_.back()));
    size_t source_position = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        while (source_position < text_points.size() &&
               space(text_points[source_position].value)) ++source_position;
        CaptionSegment segment;
        CaptionDisplayState state = CaptionDisplayState::Waiting;
        bool changed = true;
        if (i < prefix || i >= parts.size() - suffix || revision) {
            const size_t old_i = i < prefix ? i : i >= parts.size() - suffix ?
                                 segments_.size() - (parts.size() - i) : prefix;
            segment = segments_[old_i];
            state = states_[old_i];
            changed = segment.source_text != parts[i] || segment.is_final != is_final ||
                      segment.order_key.position != i;
            if (segment.source_text != parts[i]) allocate(segment.source_revision);
        } else {
            segment.key = {generation_, utterance_, allocate(next_segment_)};
            segment.source_revision = 1;
            segment.first_seen_ms = source_position < observed_prefix &&
                                    source_position < unchanged_since_.size() ?
                                    unchanged_since_[source_position] : now_ms;
            segment.replaces = replaces;
        }
        segment.order_key = {utterance_, i};
        segment.source_text = parts[i];
        segment.is_final = is_final;
        next.push_back(segment);
        next_states.push_back(state);
        if (changed) out.upserts.push_back(std::move(segment));
        source_position += points(parts[i]).size();
    }
    segments_ = std::move(next);
    states_ = std::move(next_states);
    if (is_final) tombstones_ = 0;
    return out;
}

}
