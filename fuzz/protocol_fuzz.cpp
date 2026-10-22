#include "protocol/messages.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    for (const azl::Encoding encoding : {azl::Encoding::MessagePack, azl::Encoding::Json})
        if (const std::optional<azl::Message> message = azl::decode(data, size, encoding)) {
            azl::encode(*message, azl::Encoding::MessagePack);
            azl::encode(*message, azl::Encoding::Json);
        }
    return 0;
}
