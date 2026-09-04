#pragma once
#include <string>

// Writes caption text into an OBS text source by name (libobs-dependent).
// Spec: docs/specs/001-caption-translation-pipeline.md §4.6, criterion 11.
namespace lt {

// Sets the "text" setting of the text source named `source_name`. No-op when
// the name is empty. When no source of that name exists, logs a warning once
// per name and remembers it for caption_output_missing_source().
void caption_output_write(const std::string &source_name, const std::string &text);

// Name of the most recently requested source that could not be found, or ""
// when the last write succeeded. Used for the filter's status text.
std::string caption_output_missing_source();

}
