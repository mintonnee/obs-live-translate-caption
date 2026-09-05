#include <catch2/catch_test_macros.hpp>
#include "ring-buffer.hpp"
#include <vector>

using lt::ByteRingBuffer;

TEST_CASE("write then read returns same bytes")
{
    ByteRingBuffer rb(1024);
    std::vector<uint8_t> in{1, 2, 3, 4};
    REQUIRE(rb.write(in.data(), in.size()) == 4);
    std::vector<uint8_t> out(4);
    REQUIRE(rb.read(out.data(), 4) == 4);
    REQUIRE(out == in);
    REQUIRE(rb.size() == 0);
}

TEST_CASE("read returns only what is available")
{
    ByteRingBuffer rb(1024);
    std::vector<uint8_t> in{9, 8};
    rb.write(in.data(), in.size());
    std::vector<uint8_t> out(10);
    REQUIRE(rb.read(out.data(), 10) == 2);
}

TEST_CASE("overflow drops oldest bytes")
{
    ByteRingBuffer rb(4);
    std::vector<uint8_t> a{1, 2, 3, 4};
    rb.write(a.data(), a.size());
    std::vector<uint8_t> b{5, 6};
    rb.write(b.data(), b.size());
    std::vector<uint8_t> out(4);
    REQUIRE(rb.read(out.data(), 4) == 4);
    REQUIRE(out == std::vector<uint8_t>{3, 4, 5, 6});
}

TEST_CASE("writing more than capacity keeps only newest capacity bytes")
{
    ByteRingBuffer rb(3);
    std::vector<uint8_t> a{1, 2, 3, 4, 5};
    rb.write(a.data(), a.size());
    std::vector<uint8_t> out(3);
    REQUIRE(rb.read(out.data(), 3) == 3);
    REQUIRE(out == std::vector<uint8_t>{3, 4, 5});
}

TEST_CASE("keep_last trims to the newest bytes")
{
    ByteRingBuffer rb(16000);
    std::vector<uint8_t> pattern(10000);
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<uint8_t>(i % 251);
    rb.write(pattern.data(), pattern.size());

    rb.keep_last(4000);
    REQUIRE(rb.size() == 4000);

    std::vector<uint8_t> out(4000);
    REQUIRE(rb.read(out.data(), 4000) == 4000);
    std::vector<uint8_t> expected(pattern.begin() + 6000, pattern.end());
    REQUIRE(out == expected);
}

TEST_CASE("keep_last with n >= size leaves buffer unchanged")
{
    ByteRingBuffer rb(1024);
    std::vector<uint8_t> in{1, 2, 3, 4, 5};
    rb.write(in.data(), in.size());

    rb.keep_last(in.size());
    REQUIRE(rb.size() == in.size());

    rb.keep_last(in.size() + 100);
    REQUIRE(rb.size() == in.size());

    std::vector<uint8_t> out(in.size());
    REQUIRE(rb.read(out.data(), in.size()) == in.size());
    REQUIRE(out == in);
}

TEST_CASE("keep_last(0) empties the buffer")
{
    ByteRingBuffer rb(1024);
    std::vector<uint8_t> in{1, 2, 3, 4};
    rb.write(in.data(), in.size());

    rb.keep_last(0);
    REQUIRE(rb.size() == 0);
}

TEST_CASE("keep_last on an empty buffer is a no-op")
{
    ByteRingBuffer rb(1024);
    rb.keep_last(0);
    REQUIRE(rb.size() == 0);
    rb.keep_last(100);
    REQUIRE(rb.size() == 0);
}

TEST_CASE("write and read still work after keep_last")
{
    ByteRingBuffer rb(16000);
    std::vector<uint8_t> pattern(10000);
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<uint8_t>(i & 0xFF);
    rb.write(pattern.data(), pattern.size());
    rb.keep_last(4000);

    std::vector<uint8_t> more(100);
    for (size_t i = 0; i < more.size(); ++i)
        more[i] = static_cast<uint8_t>(200 + i);
    rb.write(more.data(), more.size());
    REQUIRE(rb.size() == 4100);

    std::vector<uint8_t> out(4100);
    REQUIRE(rb.read(out.data(), 4100) == 4100);
    std::vector<uint8_t> expected(pattern.begin() + 6000, pattern.end());
    expected.insert(expected.end(), more.begin(), more.end());
    REQUIRE(out == expected);
}
