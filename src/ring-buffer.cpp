#include "ring-buffer.hpp"
#include <algorithm>
#include <cstring>

namespace lt {

size_t ByteRingBuffer::write(const uint8_t *data, size_t len)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (len >= capacity_) {
        buf_.assign(data + (len - capacity_), data + len);
        return len;
    }
    buf_.insert(buf_.end(), data, data + len);
    if (buf_.size() > capacity_) {
        size_t drop = buf_.size() - capacity_;
        buf_.erase(buf_.begin(), buf_.begin() + drop);
    }
    return len;
}

size_t ByteRingBuffer::read(uint8_t *out, size_t len)
{
    std::lock_guard<std::mutex> lock(mtx_);
    size_t n = std::min(len, buf_.size());
    if (n) {
        std::memcpy(out, buf_.data(), n);
        buf_.erase(buf_.begin(), buf_.begin() + n);
    }
    return n;
}

size_t ByteRingBuffer::size()
{
    std::lock_guard<std::mutex> lock(mtx_);
    return buf_.size();
}

void ByteRingBuffer::clear()
{
    std::lock_guard<std::mutex> lock(mtx_);
    buf_.clear();
}

void ByteRingBuffer::keep_last(size_t bytes)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (buf_.size() > bytes) {
        size_t drop = buf_.size() - bytes;
        buf_.erase(buf_.begin(), buf_.begin() + drop);
    }
}

}
