#include "import/xml.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    try {
        azl::parseXml(std::string(reinterpret_cast<const char*>(data), size));
    } catch (const std::runtime_error&) {
    }
    return 0;
}
