#include "caption-wrap.hpp"

#include <cstddef>
#include <cstdint>

// Slice S1 of docs/specs/002-caption-text-box-limits.md (§4.2, criteria 1-6
// and 9). Self-contained UTF-8 handling: no ICU, no extra dependencies.
namespace lt {
namespace {

// U+2026 HORIZONTAL ELLIPSIS, display width 1.
const char kEllipsis[] = "\xE2\x80\xA6";

struct CodePoint {
    uint32_t cp;
    size_t bytes;
};

// Decodes one code point at `i`. An invalid or truncated sequence yields
// U+FFFD and advances a single byte, so malformed input still terminates and
// counts as width 1 per byte.
CodePoint decode_utf8(std::string_view s, size_t i)
{
    const uint32_t c0 = static_cast<unsigned char>(s[i]);
    const auto cont = [&](size_t k) -> bool {
        return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0u) == 0x80u;
    };
    const auto tail = [&](size_t k) -> uint32_t {
        return static_cast<unsigned char>(s[i + k]) & 0x3Fu;
    };

    if (c0 < 0x80u) return {c0, 1};
    if (c0 >= 0xC2u && c0 <= 0xDFu && cont(1)) {
        return {((c0 & 0x1Fu) << 6) | tail(1), 2};
    }
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

// East Asian wide/fullwidth ranges of spec §3 (a simplification of Unicode
// TR#11 W/F). Everything outside them, emoji included, approximates to 1.
bool is_wide(uint32_t cp)
{
    return (cp >= 0x1100u && cp <= 0x115Fu) || (cp >= 0x2E80u && cp <= 0x303Eu) ||
           (cp >= 0x3041u && cp <= 0x33FFu) || (cp >= 0x3400u && cp <= 0x4DBFu) ||
           (cp >= 0x4E00u && cp <= 0x9FFFu) || (cp >= 0xA000u && cp <= 0xA4CFu) ||
           (cp >= 0xAC00u && cp <= 0xD7A3u) || (cp >= 0xF900u && cp <= 0xFAFFu) ||
           (cp >= 0xFE30u && cp <= 0xFE4Fu) || (cp >= 0xFF00u && cp <= 0xFF60u) ||
           (cp >= 0xFFE0u && cp <= 0xFFE6u) || (cp >= 0x20000u && cp <= 0x2FFFDu) ||
           (cp >= 0x30000u && cp <= 0x3FFFDu);
}

int code_point_width(uint32_t cp)
{
    if (cp >= 0x0300u && cp <= 0x036Fu) return 0; // combining marks
    return is_wide(cp) ? 2 : 1;
}

// Break opportunities: ASCII space and U+3000 IDEOGRAPHIC SPACE (which is also
// wide, so it costs 2 display units while it sits inside a line).
bool is_break_space(uint32_t cp)
{
    return cp == 0x0020u || cp == 0x3000u;
}

// Closing punctuation that must not start a line (criterion 6):
// 。 、 ， ． , . ! ? ！ ？ 」 』 ） )
bool is_closing_punct(uint32_t cp)
{
    switch (cp) {
    case 0x3002u: // 。
    case 0x3001u: // 、
    case 0xFF0Cu: // ，
    case 0xFF0Eu: // ．
    case 0x002Cu: // ,
    case 0x002Eu: // .
    case 0x0021u: // !
    case 0x003Fu: // ?
    case 0xFF01u: // ！
    case 0xFF1Fu: // ？
    case 0x300Du: // 」
    case 0x300Fu: // 』
    case 0xFF09u: // ）
    case 0x0029u: // )
        return true;
    default:
        return false;
    }
}

int clamp_max_width(int max_width)
{
    return max_width < 1 ? 1 : max_width;
}

int span_width(std::string_view s)
{
    int width = 0;
    size_t i = 0;
    while (i < s.size()) {
        const CodePoint c = decode_utf8(s, i);
        width += code_point_width(c.cp);
        i += c.bytes;
    }
    return width;
}

// Appends `word` (no spaces inside) to `line`, hard-splitting it between code
// points at the width boundary (rule 3) and pulling a single closing
// punctuation char back onto the finished line (rule 4). Finished lines go to
// `out`; the remainder stays in `line` so following words can join it.
void append_word(std::string_view word, int max_width, std::string &line, int &line_width,
                 std::vector<std::string> &out)
{
    size_t i = 0;
    while (i < word.size()) {
        const CodePoint c = decode_utf8(word, i);
        const int w = code_point_width(c.cp);
        // A code point is never split, so a line that is still empty always
        // takes the next code point even if it alone exceeds max_width.
        if (!line.empty() && line_width + w > max_width) {
            if (is_closing_punct(c.cp)) {
                // Pull exactly one closing punctuation char up; a second one
                // starts the next line instead of chaining.
                line.append(word.substr(i, c.bytes));
                line_width += w;
                i += c.bytes;
            }
            out.push_back(line);
            line.clear();
            line_width = 0;
            continue;
        }
        line.append(word.substr(i, c.bytes));
        line_width += w;
        i += c.bytes;
    }
}

// Wraps one newline-free paragraph. Returns no lines for empty or blank input.
std::vector<std::string> wrap_paragraph(std::string_view text, int max_width)
{
    std::vector<std::string> out;
    std::string line;
    int line_width = 0;
    std::string_view separator;  // spaces between the previous word and the next
    int separator_width = 0;

    size_t i = 0;
    while (i < text.size()) {
        const size_t space_begin = i;
        int space_width = 0;
        while (i < text.size()) {
            const CodePoint c = decode_utf8(text, i);
            if (!is_break_space(c.cp)) break;
            space_width += code_point_width(c.cp);
            i += c.bytes;
        }
        if (i > space_begin) {
            separator = text.substr(space_begin, i - space_begin);
            separator_width = space_width;
        }
        if (i >= text.size()) break; // trailing spaces are dropped

        const size_t word_begin = i;
        int word_width = 0;
        while (i < text.size()) {
            const CodePoint c = decode_utf8(text, i);
            if (is_break_space(c.cp)) break;
            word_width += code_point_width(c.cp);
            i += c.bytes;
        }
        const std::string_view word = text.substr(word_begin, i - word_begin);

        if (line.empty()) {
            // Leading spaces of a line are dropped with the separator.
            append_word(word, max_width, line, line_width, out);
        } else if (line_width + separator_width + word_width <= max_width) {
            line.append(separator);
            line_width += separator_width;
            append_word(word, max_width, line, line_width, out);
        } else {
            // Rule 2: break at the last space that keeps the line in width.
            out.push_back(line);
            line.clear();
            line_width = 0;
            append_word(word, max_width, line, line_width, out);
        }
        separator = std::string_view();
        separator_width = 0;
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

}

int display_width(std::string_view utf8)
{
    return span_width(utf8);
}

std::vector<std::string> wrap_text(std::string_view utf8, int max_width)
{
    const int width = clamp_max_width(max_width);

    std::string text;
    text.reserve(utf8.size());
    for (const char ch : utf8) {
        if (ch != '\r') text.push_back(ch);
    }

    std::vector<std::string> lines;
    size_t begin = 0;
    for (;;) {
        const size_t nl = text.find('\n', begin);
        const size_t end = (nl == std::string::npos) ? text.size() : nl;
        std::vector<std::string> wrapped =
            wrap_paragraph(std::string_view(text).substr(begin, end - begin), width);
        if (wrapped.empty()) {
            lines.emplace_back(); // a blank paragraph is still a line
        } else {
            for (std::string &line : wrapped) lines.push_back(std::move(line));
        }
        if (nl == std::string::npos) break;
        begin = nl + 1;
    }

    // A trailing '\n' terminates the last line rather than starting an empty
    // one; a lone blank line means the input held no printable text.
    if (!text.empty() && text.back() == '\n' && !lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    if (lines.size() == 1 && lines.front().empty()) lines.clear();
    return lines;
}

std::vector<std::string> wrap_tail(std::string_view utf8, int max_width, int max_lines)
{
    const size_t keep = static_cast<size_t>(max_lines < 1 ? 1 : max_lines);
    std::vector<std::string> lines = wrap_text(utf8, max_width);
    if (lines.size() <= keep) return lines;
    return std::vector<std::string>(lines.end() - static_cast<ptrdiff_t>(keep), lines.end());
}

std::string truncate_with_ellipsis(const std::string &line, int max_width)
{
    if (max_width <= 1) return std::string(kEllipsis);

    std::vector<size_t> offsets;
    int width = 0;
    size_t i = 0;
    while (i < line.size()) {
        const CodePoint c = decode_utf8(line, i);
        offsets.push_back(i);
        width += code_point_width(c.cp);
        i += c.bytes;
    }

    size_t end = line.size();
    size_t count = offsets.size();
    while (count > 0 && width + 1 > max_width) {
        --count;
        const CodePoint c = decode_utf8(line, offsets[count]);
        width -= code_point_width(c.cp);
        end = offsets[count];
    }
    return line.substr(0, end) + kEllipsis;
}

std::string join_lines(const std::vector<std::string> &lines)
{
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) out.push_back('\n');
        out += lines[i];
    }
    return out;
}

}
