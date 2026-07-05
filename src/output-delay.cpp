#include "output-delay.hpp"

namespace lt {

size_t output_delay_bytes(uint32_t delay_ms, uint32_t sample_rate,
                          uint16_t bytes_per_frame)
{
    return static_cast<size_t>(delay_ms) * sample_rate * bytes_per_frame / 1000;
}

OutputDelayBuffer::OutputDelayBuffer(size_t delay_bytes)
    : delay_bytes_(delay_bytes)
{}

std::vector<uint8_t> OutputDelayBuffer::push(const uint8_t *data, size_t len)
{
    if (delay_bytes_ == 0)
        return std::vector<uint8_t>(data, data + len);

    buffered_.insert(buffered_.end(), data, data + len);
    if (buffered_.size() <= delay_bytes_)
        return {};

    size_t release = buffered_.size() - delay_bytes_;
    std::vector<uint8_t> out(buffered_.begin(), buffered_.begin() + release);
    buffered_.erase(buffered_.begin(), buffered_.begin() + release);
    return out;
}

std::vector<uint8_t> OutputDelayBuffer::push(const std::vector<uint8_t> &data)
{
    return push(data.data(), data.size());
}

void OutputDelayBuffer::set_delay_bytes(size_t delay_bytes)
{
    if (delay_bytes_ != delay_bytes) {
        delay_bytes_ = delay_bytes;
        reset();
    }
}

void OutputDelayBuffer::reset()
{
    buffered_.clear();
}

}
