#include <catch2/catch_test_macros.hpp>
#include "caption-wrap.hpp"
#include <string>
#include <vector>

// Slice S1 of docs/specs/002-caption-text-box-limits.md: display width, line
// wrapping, tail lines and ellipsis truncation (criteria 1-6 and 9).
using lt::display_width;
using lt::join_lines;
using lt::truncate_with_ellipsis;
using lt::wrap_tail;
using lt::wrap_text;

using Lines = std::vector<std::string>;

namespace {

// Rejects truncated or malformed UTF-8 so wrapped lines can be checked for
// code point splitting (criterion 3).
bool is_valid_utf8(const std::string &s)
{
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return false;
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

std::string repeat(const std::string &unit, int times)
{
    std::string out;
    for (int i = 0; i < times; ++i) out += unit;
    return out;
}

}

// Criterion 1.
TEST_CASE("display_width counts wide code points as two units")
{
    REQUIRE(display_width("한글ab") == 6);
    // The spec's criterion 1 writes 6 for "한글 ab", but the width table of §3
    // gives U+0020 a width of 1, so the separating space makes it 7.
    REQUIRE(display_width("한글 ab") == 7);
    REQUIRE(display_width("日本語テスト") == 12);
    REQUIRE(display_width("hello") == 5);
    REQUIRE(display_width("") == 0);
    REQUIRE(display_width("　") == 2);
    REQUIRE(display_width("！？」』）") == 10);
}

TEST_CASE("display_width ignores combining marks and counts invalid bytes as one")
{
    REQUIRE(display_width("é") == 1);
    REQUIRE(display_width("café") == 4);
    REQUIRE(display_width(std::string("\xFF\xFE")) == 2);
    REQUIRE(display_width(std::string("a\xED\x95")) == 3);
}

// Criterion 2.
TEST_CASE("wrap_text breaks Latin text at spaces only")
{
    const std::string text = "the quick brown fox jumps over the lazy dog";
    const Lines lines = wrap_text(text, 20);
    REQUIRE(lines == Lines{"the quick brown fox", "jumps over the lazy", "dog"});

    std::string rejoined;
    for (const std::string &line : lines) {
        REQUIRE(display_width(line) <= 20);
        REQUIRE(!line.empty());
        REQUIRE(line.front() != ' ');
        REQUIRE(line.back() != ' ');
        if (!rejoined.empty()) rejoined += ' ';
        rejoined += line;
    }
    REQUIRE(rejoined == text);
}

// Criterion 3.
TEST_CASE("wrap_text splits space-less Hangul at the width boundary")
{
    const std::string unit = "가나다라마";
    const std::string text = repeat(unit, 6); // 30 syllables, display width 60
    const Lines lines = wrap_text(text, 40);

    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0] == repeat(unit, 4)); // 20 syllables
    REQUIRE(lines[1] == repeat(unit, 2)); // 10 syllables
    REQUIRE(display_width(lines[0]) == 40);
    REQUIRE(display_width(lines[1]) == 20);
    for (const std::string &line : lines) {
        REQUIRE(is_valid_utf8(line));
        REQUIRE(line.size() % 3 == 0); // whole 3-byte Hangul syllables only
    }
}

// Criterion 4.
TEST_CASE("wrap_text prefers spaces in mixed Korean and Latin text")
{
    const Lines lines = wrap_text("한국어 subtitle 테스트 문장입니다", 20);
    REQUIRE(lines == Lines{"한국어 subtitle", "테스트 문장입니다"});
    for (const std::string &line : lines) {
        REQUIRE(display_width(line) <= 20);
        REQUIRE(line.front() != ' ');
        REQUIRE(line.back() != ' ');
        REQUIRE(is_valid_utf8(line));
    }
}

// Criterion 5.
TEST_CASE("wrap_text hard-splits a token longer than the width")
{
    const std::string token = repeat("abcdefghij", 6); // 60 columns, no spaces
    const Lines lines = wrap_text(token, 20);
    REQUIRE(lines.size() == 3);
    for (const std::string &line : lines) REQUIRE(display_width(line) == 20);
    REQUIRE(lines[0] + lines[1] + lines[2] == token);
}

// Criterion 6.
TEST_CASE("wrap_text keeps closing punctuation on the previous line")
{
    const Lines latin = wrap_text("abcdefghijklmnopqrst.uvwxyz", 20);
    REQUIRE(latin.size() == 2);
    REQUIRE(latin[0] == "abcdefghijklmnopqrst.");
    REQUIRE(display_width(latin[0]) == 21); // max_width + width of the pulled char
    REQUIRE(latin[1] == "uvwxyz");
    REQUIRE(latin[1].compare(0, 1, ".") != 0);

    const std::string head = repeat("가나다라마", 4); // width 40
    const Lines cjk = wrap_text(head + "。다음문장", 40);
    REQUIRE(cjk.size() == 2);
    REQUIRE(cjk[0] == head + "。");
    REQUIRE(display_width(cjk[0]) == 42);
    REQUIRE(cjk[1] == "다음문장");
}

TEST_CASE("wrap_text pulls back at most one closing punctuation per boundary")
{
    // Documented choice: exactly one closing punctuation char moves up; a
    // second one starts the next line instead of chaining.
    const Lines lines = wrap_text("abcdefghijklmnopqrst..uvw", 20);
    REQUIRE(lines == Lines{"abcdefghijklmnopqrst.", ".uvw"});
}

TEST_CASE("wrap_text honors explicit newlines")
{
    REQUIRE(wrap_text("ab\ncd", 10) == Lines{"ab", "cd"});
    REQUIRE(wrap_text("ab\n\ncd", 10) == Lines{"ab", "", "cd"});
    REQUIRE(wrap_text("ab\n", 10) == Lines{"ab"}); // a trailing newline ends the last line
    REQUIRE(wrap_text("hello world\nsecond line here", 8) ==
            Lines{"hello", "world", "second", "line", "here"});
}

TEST_CASE("wrap_text strips carriage returns")
{
    REQUIRE(wrap_text("ab\r\ncd", 10) == Lines{"ab", "cd"});
    REQUIRE(wrap_text("a\rb", 10) == Lines{"ab"});
    REQUIRE(wrap_text("  \r\n  ", 10) == Lines{"", ""});
}

TEST_CASE("wrap_text clamps a width below one")
{
    REQUIRE(wrap_text("abc", 1) == Lines{"a", "b", "c"});
    REQUIRE(wrap_text("abc", 0) == Lines{"a", "b", "c"});
    REQUIRE(wrap_text("abc", -7) == Lines{"a", "b", "c"});
    // A wide code point is never split, even when it exceeds the width alone.
    REQUIRE(wrap_text("가나", 1) == Lines{"가", "나"});
}

TEST_CASE("wrap_text returns nothing for empty or blank input")
{
    REQUIRE(wrap_text("", 20).empty());
    REQUIRE(wrap_text("    ", 20).empty());
    REQUIRE(wrap_text("　　", 20).empty());
    REQUIRE(wrap_text("\n", 20).empty());
    REQUIRE(wrap_text("\r\n", 20).empty());
}

TEST_CASE("wrap_text trims edge spaces and keeps runs inside a line")
{
    REQUIRE(wrap_text("   hello   world   ", 20) == Lines{"hello   world"});
    REQUIRE(wrap_text("hello   world", 8) == Lines{"hello", "world"});
    REQUIRE(wrap_text("a  b  c", 1) == Lines{"a", "b", "c"});
}

TEST_CASE("wrap_text treats U+3000 as a wide space and a break opportunity")
{
    REQUIRE(wrap_text("가나다　라마바", 8) == Lines{"가나다", "라마바"});
    REQUIRE(wrap_text("가나다　라마바", 20) == Lines{"가나다　라마바"});
}

// Criterion 9 (function part).
TEST_CASE("wrap_tail keeps the last lines")
{
    const std::string text = "aaaa bbbb cccc dddd eeee";
    const Lines all = wrap_text(text, 4);
    REQUIRE(all == Lines{"aaaa", "bbbb", "cccc", "dddd", "eeee"});
    REQUIRE(wrap_tail(text, 4, 2) == Lines{"dddd", "eeee"});
    REQUIRE(wrap_tail(text, 4, 1) == Lines{"eeee"});
    REQUIRE(wrap_tail(text, 4, 0) == Lines{"eeee"}); // max_lines < 1 -> 1
    REQUIRE(wrap_tail(text, 4, -3) == Lines{"eeee"});
    REQUIRE(wrap_tail(text, 4, 9) == all);
    REQUIRE(wrap_tail("", 4, 2).empty());
}

TEST_CASE("truncate_with_ellipsis fits the ellipsis inside the width")
{
    REQUIRE(truncate_with_ellipsis("abcdefghij", 5) == "abcd…");
    REQUIRE(display_width(truncate_with_ellipsis("abcdefghij", 5)) == 5);

    const std::string cut = truncate_with_ellipsis("가나다라마", 5);
    REQUIRE(cut == "가나…");
    REQUIRE(display_width(cut) == 5);
    REQUIRE(is_valid_utf8(cut));

    REQUIRE(truncate_with_ellipsis("ab", 10) == "ab…"); // already short
    REQUIRE(truncate_with_ellipsis("", 10) == "…");
    REQUIRE(truncate_with_ellipsis("abc", 1) == "…");
    REQUIRE(truncate_with_ellipsis("abc", 0) == "…");
    REQUIRE(truncate_with_ellipsis("가나다", 2) == "…");
}

TEST_CASE("join_lines joins with newlines and no trailing newline")
{
    REQUIRE(join_lines({}) == "");
    REQUIRE(join_lines({"one"}) == "one");
    REQUIRE(join_lines({"a", "b", "c"}) == "a\nb\nc");
    REQUIRE(join_lines({"a", "", "c"}) == "a\n\nc");
}

TEST_CASE("wrap_text feeds a two-line caption box through join_lines")
{
    const std::string text = "한국어 자막이 두 줄로 나뉘어 표시된다";
    const Lines lines = wrap_text(text, 20);
    REQUIRE(lines.size() >= 2);
    for (const std::string &line : lines) REQUIRE(display_width(line) <= 20);
    REQUIRE(join_lines(lines).find('\n') != std::string::npos);
    REQUIRE(join_lines(wrap_tail(text, 20, 1)) == lines.back());
}
