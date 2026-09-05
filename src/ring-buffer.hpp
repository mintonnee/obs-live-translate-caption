#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace lt {

class ByteRingBuffer {
public:
    explicit ByteRingBuffer(size_t capacity) : capacity_(capacity) {}

    size_t write(const uint8_t *data, size_t len);
    size_t read(uint8_t *out, size_t len);

    size_t size();
    void clear();
    // Drops the oldest bytes so at most `bytes` remain (no-op when the buffer
    // already holds `bytes` or fewer). Used to trim stale silence to a short
    // pre-roll before the caption session reconnects (spec 004 §4.3).
    void keep_last(size_t bytes);

private:
    size_t capacity_;
    std::vector<uint8_t> buf_;
    std::mutex mtx_;
};

}
