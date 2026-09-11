#include "translation-quality.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

namespace lt {
namespace {

bool is_whitespace_cp(uint32_t cp)
{
    return cp == 0x09u || cp == 0x0Au || cp == 0x0Du || cp == 0x20u || cp == 0xA0u ||
           cp == 0x1680u || (cp >= 0x2000u && cp <= 0x200Au) || cp == 0x2028u ||
           cp == 0x2029u || cp == 0x202Fu || cp == 0x205Fu || cp == 0x3000u;
}

bool is_digit_cp(uint32_t cp)
{
    return (cp >= '0' && cp <= '9') || (cp >= 0xFF10u && cp <= 0xFF19u);
}

bool is_ascii_letter_cp(uint32_t cp)
{
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

bool is_fullwidth_letter_cp(uint32_t cp)
{
    return (cp >= 0xFF21u && cp <= 0xFF3Au) || (cp >= 0xFF41u && cp <= 0xFF5Au);
}

bool is_latin_letter_cp(uint32_t cp)
{
    if (is_ascii_letter_cp(cp) || is_fullwidth_letter_cp(cp)) return true;
    if (cp >= 0x00C0u && cp <= 0x024Fu) return cp != 0x00D7u && cp != 0x00F7u;
    return false;
}

// Letters kept for comparison: digits are separate. Katakana middle dot is not a letter.
bool is_letter_cp(uint32_t cp)
{
    if (is_latin_letter_cp(cp) || is_hangul_cp(cp)) return true;
    if (cp >= 0x0370u && cp <= 0x03FFu) return true;
    if (cp >= 0x0400u && cp <= 0x04FFu) return true;
    if (cp >= 0x3040u && cp <= 0x309Fu) return true;
    if (cp >= 0x30A0u && cp <= 0x30FFu) return cp != 0x30FBu;
    if (cp >= 0x31F0u && cp <= 0x31FFu) return true;
    if (cp == 0x3005u || cp == 0x3006u || cp == 0x303Bu) return true;
    if (cp >= 0x3400u && cp <= 0x4DBFu) return true;
    if (cp >= 0x4E00u && cp <= 0x9FFFu) return true;
    if (cp >= 0xF900u && cp <= 0xFAFFu) return true;
    if (cp >= 0x2E80u && cp <= 0x2EFFu) return true;
    if (cp >= 0x20000u && cp <= 0x2FFFFu) return true;
    return false;
}

bool is_compare_cp(uint32_t cp)
{
    return is_digit_cp(cp) || is_letter_cp(cp);
}

uint32_t ascii_fold_cp(uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z') return cp - 'A' + 'a';
    return cp;
}

std::string_view trim_ws(std::string_view s)
{
    size_t b = 0;
    while (b < s.size()) {
        const auto cp = decode_quality_utf8(s, b);
        if (!is_whitespace_cp(cp.cp)) break;
        b += cp.bytes;
    }
    size_t e = s.size();
    while (e > b) {
        size_t i = 0;
        size_t prev = b;
        while (i < e) {
            prev = i;
            i += decode_quality_utf8(s, i).bytes;
        }
        if (!is_whitespace_cp(decode_quality_utf8(s, prev).cp)) break;
        e = prev;
    }
    return s.substr(b, e - b);
}

bool has_ci(std::string_view s, std::string_view needle)
{
    if (needle.size() > s.size()) return false;
    for (size_t i = 0; i + needle.size() <= s.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < needle.size(); ++k) {
            auto a = static_cast<unsigned char>(s[i + k]);
            auto b = static_cast<unsigned char>(needle[k]);
            if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b - 'A' + 'a');
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

bool is_url_charset_cp(uint32_t cp)
{
    if (is_ascii_alnum_cp(cp)) return true;
    switch (cp) {
    case '-':
    case '.':
    case '_':
    case '~':
    case ':':
    case '/':
    case '?':
    case '#':
    case '[':
    case ']':
    case '@':
    case '!':
    case '$':
    case '&':
    case '\'':
    case '(':
    case ')':
    case '*':
    case '+':
    case ',':
    case ';':
    case '=':
    case '%':
        return true;
    default:
        return false;
    }
}

bool is_url_token(std::string_view s)
{
    const auto t = trim_ws(s);
    if (t.empty()) return false;
    for (size_t i = 0; i < t.size();) {
        const auto cp = decode_quality_utf8(t, i);
        if (is_whitespace_cp(cp.cp)) return false;
        i += cp.bytes;
    }
    if (has_ci(t, "://") || has_ci(t, "www.")) return true;
    bool dot = false;
    bool letter = false;
    for (size_t i = 0; i < t.size();) {
        const auto cp = decode_quality_utf8(t, i);
        i += cp.bytes;
        if (is_hangul_cp(cp.cp) || (is_letter_cp(cp.cp) && !is_latin_letter_cp(cp.cp)))
            return false;
        if (!is_url_charset_cp(cp.cp)) return false;
        if (cp.cp == '.') dot = true;
        if (is_ascii_letter_cp(cp.cp)) letter = true;
    }
    return dot && letter;
}

bool is_digits_symbols_or_url(std::string_view s)
{
    if (is_url_token(s)) return true;
    for (size_t i = 0; i < s.size();) {
        const auto cp = decode_quality_utf8(s, i);
        i += cp.bytes;
        if (is_whitespace_cp(cp.cp)) continue;
        if (is_letter_cp(cp.cp)) return false;
    }
    return true;
}

std::vector<uint32_t> normalize_compare(std::string_view s)
{
    std::vector<uint32_t> out;
    bool pending_space = false;
    for (size_t i = 0; i < s.size();) {
        const auto cp = decode_quality_utf8(s, i);
        i += cp.bytes;
        if (is_whitespace_cp(cp.cp)) {
            if (!out.empty()) pending_space = true;
            continue;
        }
        if (!is_compare_cp(cp.cp)) continue;
        if (pending_space) {
            out.push_back(0x20u);
            pending_space = false;
        }
        out.push_back(ascii_fold_cp(cp.cp));
    }
    return out;
}

size_t count_hangul(std::string_view s)
{
    size_t n = 0;
    for (size_t i = 0; i < s.size();) {
        const auto cp = decode_quality_utf8(s, i);
        i += cp.bytes;
        if (is_hangul_cp(cp.cp)) ++n;
    }
    return n;
}

size_t count_general_letters(const std::vector<uint32_t> &cps)
{
    size_t n = 0;
    for (uint32_t cp : cps) {
        if (is_letter_cp(cp)) ++n;
    }
    return n;
}

bool source_has_run(std::string_view source, const std::vector<uint32_t> &run)
{
    if (run.empty()) return false;
    std::vector<uint32_t> cps;
    cps.reserve(utf8_codepoints(source));
    for (size_t i = 0; i < source.size();) {
        const auto cp = decode_quality_utf8(source, i);
        i += cp.bytes;
        cps.push_back(cp.cp);
    }
    if (run.size() > cps.size()) return false;
    for (size_t i = 0; i + run.size() <= cps.size(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < run.size(); ++k) {
            if (cps[i + k] != run[k]) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

bool has_hangul_residue(std::string_view source, std::string_view output)
{
    std::vector<uint32_t> run;
    for (size_t i = 0; i < output.size();) {
        const auto cp = decode_quality_utf8(output, i);
        const size_t next = i + cp.bytes;
        if (is_hangul_cp(cp.cp)) {
            run.push_back(cp.cp);
        } else if (!run.empty()) {
            if (run.size() >= kQualityMinHangulResidueRun && source_has_run(source, run))
                return true;
            run.clear();
        }
        i = next;
    }
    return run.size() >= kQualityMinHangulResidueRun && source_has_run(source, run);
}

size_t count_hangul_from_source(std::string_view source, std::string_view output)
{
    std::vector<uint32_t> src_hangul;
    for (size_t i = 0; i < source.size();) {
        const auto cp = decode_quality_utf8(source, i);
        i += cp.bytes;
        if (is_hangul_cp(cp.cp)) src_hangul.push_back(cp.cp);
    }
    size_t n = 0;
    for (size_t i = 0; i < output.size();) {
        const auto cp = decode_quality_utf8(output, i);
        const size_t next = i + cp.bytes;
        if (is_hangul_cp(cp.cp)) {
            for (uint32_t s : src_hangul) {
                if (s == cp.cp) {
                    ++n;
                    break;
                }
            }
        }
        i = next;
    }
    return n;
}

size_t levenshtein(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b)
{
    const size_t n = a.size();
    const size_t m = b.size();
    if (n == 0) return m;
    if (m == 0) return n;
    std::vector<size_t> prev(m + 1);
    std::vector<size_t> curr(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            const size_t del = prev[j] + 1;
            const size_t ins = curr[j - 1] + 1;
            const size_t sub = prev[j - 1] + cost;
            curr[j] = del < ins ? (del < sub ? del : sub) : (ins < sub ? ins : sub);
        }
        prev.swap(curr);
    }
    return prev[m];
}

bool is_short_latin_name(const std::vector<uint32_t> &cps)
{
    size_t letters = 0;
    for (uint32_t cp : cps) {
        if (cp == 0x20u || is_digit_cp(cp)) continue;
        if (!is_latin_letter_cp(cp)) return false;
        ++letters;
    }
    return letters > 0 && letters < kQualityMinLettersForNearCopy;
}

} // namespace

QualityJudgement inspect_translation_quality(const QualityInspectInput &input)
{
    const auto t0 = std::chrono::steady_clock::now();
    QualityJudgement j;
    j.source_codepoints = utf8_codepoints(input.source);
    j.output_codepoints = utf8_codepoints(input.output);

    const auto done = [&](QualityVerdict verdict, QualitySuspectReason reason) {
        j.verdict = verdict;
        j.reason = reason;
        const auto us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                  t0)
                .count();
        j.detect_us = us < 0 ? 0 : static_cast<uint64_t>(us);
        return j;
    };

    if (input.source.empty() || input.output.empty())
        return done(QualityVerdict::Indeterminate, QualitySuspectReason::None);

    const bool japanese = is_japanese_target(input.target_code);

    if (is_digits_symbols_or_url(input.source) && is_digits_symbols_or_url(input.output))
        return done(QualityVerdict::Normal, QualitySuspectReason::None);

    const auto compared_s = normalize_compare(input.source);
    const auto compared_o = normalize_compare(input.output);
    const bool compared_equal = compared_s == compared_o;
    const size_t source_hangul = count_hangul(input.source);

    if (japanese) {
        if (source_hangul >= kQualityMinHangulForCopy && compared_equal)
            return done(QualityVerdict::Suspected, QualitySuspectReason::SourceCopy);
        if (has_hangul_residue(input.source, input.output))
            return done(QualityVerdict::Suspected, QualitySuspectReason::HangulResidue);
    }

    if (!compared_s.empty() || !compared_o.empty()) {
        if (compared_s.size() <= kQualityMaxSimilarityCodepoints &&
            compared_o.size() <= kQualityMaxSimilarityCodepoints) {
            const size_t denom = std::max(compared_s.size(), compared_o.size());
            if (denom > 0) {
                j.similarity_computed = true;
                j.similarity = 1.0 - static_cast<double>(levenshtein(compared_s, compared_o)) /
                                         static_cast<double>(denom);
            }
        }
    }

    if (japanese && j.similarity_computed &&
        count_general_letters(compared_s) >= kQualityMinLettersForNearCopy &&
        j.similarity >= kQualityNearCopySimilarity &&
        count_hangul_from_source(input.source, input.output) >=
            kQualityMinHangulForNearCopy)
        return done(QualityVerdict::Suspected, QualitySuspectReason::NearCopy);

    if (!japanese) return done(QualityVerdict::Indeterminate, QualitySuspectReason::None);

    if (compared_equal && source_hangul < kQualityMinHangulForCopy &&
        is_short_latin_name(compared_s))
        return done(QualityVerdict::Indeterminate, QualitySuspectReason::None);

    return done(QualityVerdict::Normal, QualitySuspectReason::None);
}

}
