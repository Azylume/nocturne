#pragma once

#include "gpu/gpu.hpp"

#include <algorithm>
#include <cstring>

namespace azl::gpu {

// Fragment constants of any size for one thread's render passes. Up to Metal's 4 KiB setBytes limit they go through
// setBytes; larger ones are copied into a ring buffer. The ring is allocated on the first large call, and grows
// (retired buffers stay alive in the encoders that bound them) only for a single call bigger than the ring.
class ConstantsRing {
public:
    static constexpr size_t kInlineLimit = 4096;

    explicit ConstantsRing(const Device& device, size_t capacity = size_t(1) << 20)
        : device_(device), capacity_(capacity)
    {
    }

    void set(RenderPass& pass, uint32_t index, const void* data, size_t size)
    {
        if (size <= kInlineLimit)
            return pass.setBytes(index, data, size);
        const size_t offset = reserve(size);
        std::memcpy(static_cast<char*>(buffer_.contents()) + offset, data, size);
        pass.setBuffer(index, buffer_, offset);
    }

private:
    static constexpr size_t kAlign = 256;

    size_t reserve(size_t size)
    {
        if (!buffer_ || size > buffer_.size()) {
            capacity_ = std::max(capacity_, size);
            buffer_ = device_.makeBuffer(capacity_, Memory::Shared);
            head_ = 0;
        }
        if (head_ + size > buffer_.size())
            head_ = 0;
        const size_t offset = head_;
        head_ = (offset + size + kAlign - 1) / kAlign * kAlign;
        return offset;
    }

    Device device_;
    size_t capacity_;
    Buffer buffer_;
    size_t head_ = 0;
};

} // namespace azl::gpu
