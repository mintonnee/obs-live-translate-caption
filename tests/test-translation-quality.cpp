#include <catch2/catch_test_macros.hpp>
#include "translation-quality.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using nlohmann::json;
using namespace lt;

namespace {

std::filesystem::path fixtures_path()
{
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "translation-quality" /
           "fixtures.json";
}

std::string slurp(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

QualityVerdict parse_verdict(const std::string &s)
{
    if (s == "normal") return QualityVerdict::Normal;
    if (s == "suspected") return QualityVerdict::Suspected;
    if (s == "indeterminate") return QualityVerdict::Indeterminate;
    FAIL("unknown verdict: " << s);
    return QualityVerdict::Indeterminate;
}

QualitySuspectReason parse_reason(const std::string &s)
{
    if (s == "none") return QualitySuspectReason::None;
    if (s == "source_copy") return QualitySuspectReason::SourceCopy;
    if (s == "hangul_residue") return QualitySuspectReason::HangulResidue;
    if (s == "near_copy") return QualitySuspectReason::NearCopy;
    FAIL("unknown reason: " << s);
    return QualitySuspectReason::None;
}

const char *verdict_name(QualityVerdict v)
{
    switch (v) {
    case QualityVerdict::Normal:
        return "normal";
    case QualityVerdict::Suspected:
        return "suspected";
    case QualityVerdict::Indeterminate:
        return "indeterminate";
    }
    return "unknown";
}

std::string repeat_unit(const std::string &unit, size_t n)
{
    std::string out;
    out.reserve(unit.size() * n);
    for (size_t i = 0; i < n; ++i) out += unit;
    return out;
}

} // namespace

TEST_CASE("translation quality fixtures match expected verdicts")
{
    const auto path = fixtures_path();
    REQUIRE(std::filesystem::exists(path));
    const json fixtures = json::parse(slurp(path));
    REQUIRE(fixtures.is_array());
    REQUIRE(fixtures.size() >= 40);

    size_t saw_source_copy = 0;
    size_t saw_residue = 0;
    size_t saw_near_copy = 0;

    for (const auto &fx : fixtures) {
        const std::string name = fx.at("name").get<std::string>();
        INFO(name);
        const std::string target = fx.at("target_code").get<std::string>();
        const std::string source = fx.at("source").get<std::string>();
        const std::string output = fx.at("output").get<std::string>();
        QualityInspectInput in;
        in.target_code = target;
        in.source = source;
        in.output = output;

        const QualityJudgement j = inspect_translation_quality(in);
        const auto want_v = parse_verdict(fx.at("verdict").get<std::string>());
        const auto want_r = parse_reason(fx.at("reason").get<std::string>());

        REQUIRE(j.source_codepoints == utf8_codepoints(source));
        REQUIRE(j.output_codepoints == utf8_codepoints(output));
        REQUIRE(j.verdict == want_v);
        REQUIRE(j.reason == want_r);
        REQUIRE(std::string(quality_reason_name(j.reason)) == fx.at("reason").get<std::string>());
        REQUIRE(std::string(verdict_name(j.verdict)) == fx.at("verdict").get<std::string>());

        if (want_r == QualitySuspectReason::SourceCopy) ++saw_source_copy;
        if (want_r == QualitySuspectReason::HangulResidue) ++saw_residue;
        if (want_r == QualitySuspectReason::NearCopy) ++saw_near_copy;
    }

    REQUIRE(saw_source_copy >= 1);
    REQUIRE(saw_residue >= 1);
    REQUIRE(saw_near_copy >= 1);
}

TEST_CASE("criterion 3 source copy and non-retry exceptions")
{
    QualityInspectInput in;
    in.target_code = "ja";
    in.source = "오늘 로블록스 할까요?";
    in.output = "오늘 로블록스 할까요?";
    auto j = inspect_translation_quality(in);
    REQUIRE(j.verdict == QualityVerdict::Suspected);
    REQUIRE(j.reason == QualitySuspectReason::SourceCopy);

    in.source = "Roblox";
    in.output = "Roblox";
    j = inspect_translation_quality(in);
    REQUIRE(j.verdict == QualityVerdict::Indeterminate);
    REQUIRE(j.reason == QualitySuspectReason::None);

    in.source = "12345";
    in.output = "12345";
    j = inspect_translation_quality(in);
    REQUIRE(j.verdict == QualityVerdict::Normal);

    in.source = "로블록스";
    in.output = "로블록스";
    j = inspect_translation_quality(in);
    REQUIRE(j.verdict == QualityVerdict::Suspected);
    REQUIRE(j.reason == QualitySuspectReason::SourceCopy);
}

TEST_CASE("criterion 4 hangul residue uses source-contiguous runs")
{
    QualityInspectInput in;
    in.target_code = "ja";
    in.source = "오늘은 로블록스 할까요?";
    in.output = "今日は로블록스で遊びます。";
    const auto j = inspect_translation_quality(in);
    REQUIRE(j.verdict == QualityVerdict::Suspected);
    REQUIRE(j.reason == QualitySuspectReason::HangulResidue);
}

TEST_CASE("latin-to-latin identical is indeterminate not suspected")
{
    QualityInspectInput in;
    in.target_code = "fr";
    in.source = "The cat sits.";
    in.output = "The cat sits.";
    const auto j = inspect_translation_quality(in);
    REQUIRE(j.verdict == QualityVerdict::Indeterminate);
    REQUIRE(j.reason == QualitySuspectReason::None);
}

TEST_CASE("similarity is computed only within the 256 code-point cap")
{
    const std::string a256 = repeat_unit("a", kQualityMaxSimilarityCodepoints);
    const std::string a256b = repeat_unit("a", kQualityMaxSimilarityCodepoints - 1) + "b";
    QualityInspectInput in;
    in.target_code = "ja";
    in.source = a256;
    in.output = a256b;
    auto j = inspect_translation_quality(in);
    REQUIRE(j.similarity_computed);
    REQUIRE(j.source_codepoints == kQualityMaxSimilarityCodepoints);
    REQUIRE(j.output_codepoints == kQualityMaxSimilarityCodepoints);
    REQUIRE(j.verdict == QualityVerdict::Normal);

    const std::string a257 = repeat_unit("a", kQualityMaxSimilarityCodepoints + 1);
    in.source = a257;
    in.output = a257;
    j = inspect_translation_quality(in);
    REQUIRE_FALSE(j.similarity_computed);
    REQUIRE(j.source_codepoints == kQualityMaxSimilarityCodepoints + 1);
    REQUIRE(j.verdict == QualityVerdict::Normal);

    const std::string ga257 = repeat_unit("가", kQualityMaxSimilarityCodepoints + 1);
    in.source = ga257;
    in.output = "今日は가가가가です。";
    j = inspect_translation_quality(in);
    REQUIRE_FALSE(j.similarity_computed);
    REQUIRE(j.verdict == QualityVerdict::Suspected);
    REQUIRE(j.reason == QualitySuspectReason::HangulResidue);
}

TEST_CASE("empty inputs are indeterminate and keep raw code-point counts")
{
    QualityInspectInput in;
    in.target_code = "ja";
    in.source = "";
    in.output = "こんにちは";
    auto j = inspect_translation_quality(in);
    REQUIRE(j.verdict == QualityVerdict::Indeterminate);
    REQUIRE(j.source_codepoints == 0);
    REQUIRE(j.output_codepoints == utf8_codepoints("こんにちは"));

    in.source = "안녕";
    in.output = "";
    j = inspect_translation_quality(in);
    REQUIRE(j.verdict == QualityVerdict::Indeterminate);
    REQUIRE(j.source_codepoints == 2);
    REQUIRE(j.output_codepoints == 0);
}

TEST_CASE("Japanese input already in the target language remains normal")
{
    QualityInspectInput in;
    in.target_code = "ja";
    in.source = "今日は";
    in.output = "今日は";
    const auto j = inspect_translation_quality(in);
    REQUIRE(j.verdict == QualityVerdict::Normal);
    REQUIRE(j.reason == QualitySuspectReason::None);
}

TEST_CASE("malformed UTF-8 still terminates")
{
    const std::string bad = std::string("a\xFF\xFE") + "가";
    QualityInspectInput in;
    in.target_code = "ja";
    in.source = bad;
    in.output = bad;
    const auto j = inspect_translation_quality(in);
    REQUIRE(j.source_codepoints == utf8_codepoints(bad));
    REQUIRE(j.output_codepoints == utf8_codepoints(bad));
}

TEST_CASE("quality inspect hidden perf sample", "[.quality-perf]")
{
    const std::string source = "오늘 로블록스 할까요?";
    const std::string output = "今日はRobloxをやりましょうか？";
    QualityInspectInput in;
    in.target_code = "ja";
    in.source = source;
    in.output = output;

    REQUIRE(utf8_codepoints(source) <= kQualityMaxSimilarityCodepoints);
    REQUIRE(utf8_codepoints(output) <= kQualityMaxSimilarityCodepoints);

    for (int i = 0; i < 32; ++i) (void)inspect_translation_quality(in);

    constexpr int kN = 10000;
    std::vector<uint64_t> samples;
    samples.reserve(kN);
    const std::clock_t cpu0 = std::clock();
    const auto wall0 = std::chrono::steady_clock::now();
    QualityJudgement last;
    for (int i = 0; i < kN; ++i) {
        last = inspect_translation_quality(in);
        samples.push_back(last.detect_us);
    }
    const auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - wall0)
                             .count();
    const double cpu_ms = 1000.0 * static_cast<double>(std::clock() - cpu0) / CLOCKS_PER_SEC;

    std::sort(samples.begin(), samples.end());
    const uint64_t p50 = samples[static_cast<size_t>(kN * 50 / 100)];
    const uint64_t p95 = samples[static_cast<size_t>(kN * 95 / 100)];
    const uint64_t pmax = samples.back();

    std::cout << "quality-perf n=" << kN << " p50_us=" << p50 << " p95_us=" << p95
              << " max_us=" << pmax << " cpu_ms=" << cpu_ms << " wall_us=" << wall_us
              << " source_cp=" << last.source_codepoints << " output_cp=" << last.output_codepoints
              << " similarity_computed=" << (last.similarity_computed ? "true" : "false")
              << " reason=" << quality_reason_name(last.reason) << '\n';

    REQUIRE(samples.size() == static_cast<size_t>(kN));
    REQUIRE(last.source_codepoints == utf8_codepoints(source));
    REQUIRE(last.output_codepoints == utf8_codepoints(output));
}
