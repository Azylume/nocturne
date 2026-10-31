#include "effects/shader_layout.hpp"

namespace azl {

const char* const kGlslSamplers = R"(
layout(set = 2, binding = 0) uniform sampler lin;
layout(set = 2, binding = 1) uniform sampler pix;
layout(set = 2, binding = 2) uniform sampler textSampler;
)";

const char* const kFullscreenVertex = R"(#version 450
void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

std::string withEntry(const std::string& glsl, const std::string& entry)
{
    const size_t line = glsl.find('\n') + 1;
    return glsl.substr(0, line) + "#define ENTRY_" + entry + "\n" + glsl.substr(line);
}

} // namespace azl
