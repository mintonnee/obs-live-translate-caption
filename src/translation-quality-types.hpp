#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

// Spec 006 contracts. Value types only; no I/O. Inspection lives in translation-quality.cpp.
namespace lt {

enum class QualityVerdict { Normal, Suspected, Indeterminate };

enum class QualitySuspectReason {
    None,
    SourceCopy,
    HangulResidue,
    NearCopy
};

enum class QualityRetryOutcome {
    Accepted,
    StillSuspected,
    Indeterminate,
    Expired,
    Superseded,
    Disabled,
    QueueFull,
    HttpFailure
};

struct QualityInspectInput {
    std::string_view target_code;
    std::string_view source;
    std::string_view output;
};

struct QualityJudgement {
    QualityVerdict verdict = QualityVerdict::Indeterminate;
    QualitySuspectReason reason = QualitySuspectReason::None;
    bool similarity_computed = false;
    double similarity = 0.0;
    uint64_t detect_us = 0;
    size_t source_codepoints = 0;
    size_t output_codepoints = 0;
};

constexpr size_t kQualityMaxSimilarityCodepoints = 256;
constexpr size_t kQualityMinHangulForCopy = 2;
constexpr size_t kQualityMinHangulResidueRun = 4;
constexpr size_t kQualityMinHangulForNearCopy = 2;
constexpr size_t kQualityMinLettersForNearCopy = 8;
constexpr double kQualityNearCopySimilarity = 0.90;
constexpr size_t kQualityMaxRetryQueued = 4;
constexpr size_t kQualityMaxRetryInFlight = 1;
constexpr uint64_t kQualityMinRemainingMs = 1000;

struct QualityCodePoint {
    uint32_t cp = 0;
    size_t bytes = 1;
};

inline QualityCodePoint decode_quality_utf8(std::string_view s, size_t i)
{
    if (i >= s.size()) return {0xFFFDu, 1};
    const uint32_t c0 = static_cast<unsigned char>(s[i]);
    const auto cont = [&](size_t k) {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0u) == 0x80u;
    };
    const auto tail = [&](size_t k) -> uint32_t {
        return static_cast<unsigned char>(s[i + k]) & 0x3Fu;
    };
    if (c0 < 0x80u) return {c0, 1};
    if (c0 >= 0xC2u && c0 <= 0xDFu && cont(1))
        return {((c0 & 0x1Fu) << 6) | tail(1), 2};
    if (c0 >= 0xE0u && c0 <= 0xEFu && cont(1) && cont(2)) {
        const uint32_t cp = ((c0 & 0x0Fu) << 12) | (tail(1) << 6) | tail(2);
        if (cp >= 0x800u) return {cp, 3};
    }
    if (c0 >= 0xF0u && c0 <= 0xF4u && cont(1) && cont(2) && cont(3)) {
        const uint32_t cp = ((c0 & 0x07u) << 18) | (tail(1) << 12) | (tail(2) << 6) | tail(3);
        if (cp >= 0x10000u && cp <= 0x10FFFFu) return {cp, 4};
    }
    return {0xFFFDu, 1};
}

inline size_t utf8_codepoints(std::string_view s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        i += decode_quality_utf8(s, i).bytes;
        ++n;
    }
    return n;
}

inline bool is_hangul_cp(uint32_t cp)
{
    return (cp >= 0x1100u && cp <= 0x11FFu) || (cp >= 0x3130u && cp <= 0x318Fu) ||
           (cp >= 0xA960u && cp <= 0xA97Fu) || (cp >= 0xAC00u && cp <= 0xD7A3u) ||
           (cp >= 0xD7B0u && cp <= 0xD7FFu);
}

inline bool is_ascii_alnum_cp(uint32_t cp)
{
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

inline std::string_view language_primary_subtag(std::string_view code)
{
    const auto pos = code.find_first_of("-_");
    return pos == std::string_view::npos ? code : code.substr(0, pos);
}

inline bool ascii_ieq(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        auto ca = static_cast<unsigned char>(a[i]);
        auto cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

inline bool is_japanese_target(std::string_view code)
{
    return ascii_ieq(language_primary_subtag(code), "ja");
}

inline const char *quality_reason_name(QualitySuspectReason reason)
{
    switch (reason) {
    case QualitySuspectReason::SourceCopy:
        return "source_copy";
    case QualitySuspectReason::HangulResidue:
        return "hangul_residue";
    case QualitySuspectReason::NearCopy:
        return "near_copy";
    case QualitySuspectReason::None:
        return "none";
    }
    return "none";
}

}
