#include "gpu/glsl.hpp"

#include <stdexcept>

namespace azl::gpu {

#if defined(__APPLE__)

ShaderSource compileGlsl(const std::vector<ShaderEntry>& entries)
{
    return {.msl = mslSource(entries)};
}

std::string entryPoint(const std::string& entry)
{
    return mslFunction(entry);
}

#else

ShaderSource compileGlsl(const std::vector<ShaderEntry>& entries)
{
    ShaderSource source;
    for (const ShaderEntry& entry : entries) {
        std::string error;
        std::vector<uint32_t> code = glslToSpirv(entry.stage, entry.glsl, error);
        if (code.empty())
            throw std::runtime_error("shader compile failed: " + entry.name + ": " + error);
        source.spirv.push_back({entry.name, std::move(code)});
    }
    return source;
}

std::string entryPoint(const std::string& entry)
{
    return entry;
}

#endif

} // namespace azl::gpu
