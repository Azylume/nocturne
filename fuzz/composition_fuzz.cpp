#include "document/composition_file.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const nlohmann::json j = nlohmann::json::parse(data, data + size, nullptr, false);
    if (j.is_discarded())
        return 0;
    std::vector<std::string> warnings;
    try {
        const nlohmann::json migrated = azl::migrateCompositionFile(j, warnings);
        azl::toJson(azl::compositionFileFromJson(j, warnings));
        azl::compositionFileFromJson(migrated);
    } catch (const std::exception&) {
    }
    return 0;
}
