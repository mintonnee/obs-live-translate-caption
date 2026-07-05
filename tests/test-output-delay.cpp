#include <catch2/catch_test_macros.hpp>
#include "output-delay.hpp"

#include <cstdint>
#include <vector>

using namespace lt;

namespace {

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> values)
{
    return std::vector<uint8_t>(values);
}

}

TEST_CASE("output delay holds audio until the configured delay is buffered")
{
    OutputDelayBuffer delay(4);

    auto released = delay.push(bytes({1, 2}));
    REQUIRE(released.empty());

    released = delay.push(bytes({3, 4}));
    REQUIRE(released.empty());
}

TEST_CASE("output delay releases only audio beyond the configured delay")
{
    OutputDelayBuffer delay(4);

    auto released = delay.push(bytes({1, 2, 3, 4, 5, 6}));
    REQUIRE(released == bytes({1, 2}));

    released = delay.push(bytes({7, 8, 9}));
    REQUIRE(released == bytes({3, 4, 5}));
}

TEST_CASE("output delay with zero delay passes audio through immediately")
{
    OutputDelayBuffer delay(0);

    auto released = delay.push(bytes({1, 2, 3}));
    REQUIRE(released == bytes({1, 2, 3}));
}

TEST_CASE("output delay reset drops pending delayed audio")
{
    OutputDelayBuffer delay(4);

    REQUIRE(delay.push(bytes({1, 2, 3, 4})).empty());
    delay.reset();

    REQUIRE(delay.push(bytes({5, 6})).empty());
    REQUIRE(delay.push(bytes({7, 8})).empty());
    REQUIRE(delay.push(bytes({9})) == bytes({5}));
}

TEST_CASE("output delay converts milliseconds to PCM byte delay")
{
    REQUIRE(output_delay_bytes(0, 24000, 2) == 0);
    REQUIRE(output_delay_bytes(1000, 24000, 2) == 48000);
    REQUIRE(output_delay_bytes(30500, 24000, 2) == 1464000);
}
