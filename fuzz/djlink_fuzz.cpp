#include "djlink/djlink_packets.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    for (const uint16_t port : {uint16_t(50000), uint16_t(50001), uint16_t(50002)})
        azl::djlink::parse(port, data, size);
    return 0;
}
