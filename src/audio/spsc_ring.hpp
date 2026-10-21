#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace azl {

// A wait-free single-producer single-consumer ring of trivially copyable items. Capacity is a power of two.
template <typename T, size_t Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0);

public:
    size_t writable() const { return Capacity - (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire)); }
    size_t readable() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed); }

    // Producer only. False, with nothing written, when there is no room for all of them.
    bool write(const T* items, size_t count)
    {
        if (count > writable())
            return false;
        const uint64_t head = head_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i)
            items_[(head + i) & (Capacity - 1)] = items[i];
        head_.store(head + count, std::memory_order_release);
        return true;
    }

    // Consumer only. Reads up to `count` items and returns how many it read.
    size_t read(T* items, size_t count)
    {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const size_t n = std::min(count, size_t(head_.load(std::memory_order_acquire) - tail));
        for (size_t i = 0; i < n; ++i)
            items[i] = items_[(tail + i) & (Capacity - 1)];
        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

private:
    std::array<T, Capacity> items_{};
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

} // namespace azl
