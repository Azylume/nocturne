#include "media/snappy.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (const std::optional<size_t> length = azl::snappyLength(data, size); length && *length <= (64u << 20)) {
        std::vector<uint8_t> out(*length);
        azl::snappyDecompress(data, size, out.data(), out.size());
    }
    const std::vector<uint8_t> packed = azl::snappyCompress(data, size);
    std::vector<uint8_t> back(size);
    if (azl::snappyLength(packed.data(), packed.size()) != size ||
        !azl::snappyDecompress(packed.data(), packed.size(), back.data(), back.size()) ||
        !std::equal(back.begin(), back.end(), data))
        __builtin_trap();
    return 0;
}
