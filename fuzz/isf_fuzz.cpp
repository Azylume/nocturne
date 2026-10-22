#include "effects/isf.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    azl::parseIsf({"Fuzz", std::string(reinterpret_cast<const char*>(data), size), true});
    return 0;
}
