#pragma once
#include <string>
#include <string_view>
#include <vector>

// Display-width accounting and line wrapping for caption text. Pure functions,
// no libobs dependency. Widths are "display units": East Asian wide/fullwidth
// code points count 2, combining marks 0, everything else 1 (see spec §3 for
// the exact ranges). Input is UTF-8; a code point is never split.
// Spec: docs/specs/002-caption-text-box-limits.md §4.2, criteria 1-6, 9.
namespace lt {

// Sum of per-code-point widths. Invalid UTF-8 bytes count 1 each.
int display_width(std::string_view utf8);

// Splits `utf8` into lines whose display width is <= max_width (max_width < 1
// is treated as 1). Rules, in priority order:
//   1. an existing '\n' always ends a line;
//   2. break at the last space (U+0020 or U+3000) that keeps the line within
//      max_width; spaces are trimmed from both ends of every line;
//   3. with no usable space, break between code points at the width boundary
//      (CJK text, long Latin words / URLs);
//   4. if the code point right after a boundary is closing punctuation
//      (。、，．,.!?！？」』）)), it stays on the previous line even though that
//      line then exceeds max_width by its width.
// Empty input -> empty vector. Lines are never empty except for explicit
// blank lines produced by consecutive '\n'.
std::vector<std::string> wrap_text(std::string_view utf8, int max_width);

// The last `max_lines` lines of wrap_text(utf8, max_width) (max_lines < 1 is
// treated as 1). Used for the growing interim transcript so the newest words
// stay visible.
std::vector<std::string> wrap_tail(std::string_view utf8, int max_width, int max_lines);

// Appends "…" (U+2026, width 1) to `line`, removing code points from the end
// first so the result's display width is <= max_width. A line that already
// fits with the ellipsis is returned with the ellipsis appended unchanged.
std::string truncate_with_ellipsis(const std::string &line, int max_width);

// Joins lines with '\n' (no trailing newline).
std::string join_lines(const std::vector<std::string> &lines);

}
