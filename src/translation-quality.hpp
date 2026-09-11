#pragma once
#include "translation-quality-types.hpp"

// Spec 006 §4.3 local untranslated-quality inspect (slice S2).
namespace lt {

// Pure local inspect. No network, no audio, no regex backtracking, no embedding API.
QualityJudgement inspect_translation_quality(const QualityInspectInput &input);

}
