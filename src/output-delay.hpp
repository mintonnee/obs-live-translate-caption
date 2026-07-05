#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lt {

size_t output_delay_bytes(uint32_t delay_ms, uint32_t sample_rate,
                          uint16_t bytes_per_frame);

class OutputDelayBuffer {
public:
    explicit OutputDelayBuffer(size_t delay_bytes);

    std::vector<uint8_t> push(const uint8_t *data, size_t len);
    std::vector<uint8_t> push(const std::vector<uint8_t> &data);
    void set_delay_bytes(size_t delay_bytes);
    size_t delay_bytes() const { return delay_bytes_; }
    void reset();

private:
    size_t delay_bytes_;
    std::vector<uint8_t> buffered_;
};

}
