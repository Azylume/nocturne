#pragma once

#include <atomic>
#include <cstdint>

namespace azl {

// Any thread, lock-free: raises `max` to `value` unless it is already at least that, even against concurrent raises.
inline void atomicMax(std::atomic<uint64_t>& max, uint64_t value)
{
    for (uint64_t seen = max.load(); value > seen;)
        if (max.compare_exchange_weak(seen, value))
            return;
}

}  // namespace azl
