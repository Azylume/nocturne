#include "gpu/vulkan_spirv.hpp"

#include "effects/shader_layout.hpp"

#include <unordered_map>
#include <unordered_set>

namespace azl::gpu::vk {

namespace {

// The SPIR-V words this reads (SPIR-V 1.0 specification, section 3).
constexpr uint32_t kMagic = 0x07230203;
constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpExecutionMode = 16;
constexpr uint32_t kOpTypeImage = 25;
constexpr uint32_t kOpTypeSampler = 26;
constexpr uint32_t kOpTypeSampledImage = 27;
constexpr uint32_t kOpTypeArray = 28;
constexpr uint32_t kOpTypeRuntimeArray = 29;
constexpr uint32_t kOpTypePointer = 32;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kDecorationBufferBlock = 3;
constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;
constexpr uint32_t kExecutionModeLocalSize = 17;
constexpr uint32_t kModelVertex = 0;
constexpr uint32_t kModelFragment = 4;
constexpr uint32_t kModelCompute = 5;
constexpr uint32_t kStorageUniformConstant = 0;
constexpr uint32_t kStorageUniform = 2;
constexpr uint32_t kStorageStorageBuffer = 12;

struct Variable {
    uint32_t id;
    uint32_t pointer;
    uint32_t storage;
};

} // namespace

bool renumber(std::vector<uint32_t>& code, SpirvInterface& out, std::string& error)
{
    if (code.size() < 5 || code[0] != kMagic) {
        error = "not SPIR-V";
        return false;
    }
    uint32_t model = kModelFragment;
    std::unordered_map<uint32_t, size_t> setWord, bindingWord;
    std::unordered_map<uint32_t, uint32_t> pointee, element;
    std::unordered_map<uint32_t, VkDescriptorType> opaque;
    std::unordered_set<uint32_t> bufferBlocks;
    std::vector<Variable> variables;
    for (size_t i = 5; i < code.size();) {
        const uint32_t op = code[i] & 0xffff, count = code[i] >> 16;
        if (count == 0 || i + count > code.size()) {
            error = "truncated SPIR-V";
            return false;
        }
        switch (op) {
        case kOpEntryPoint: model = code[i + 1]; break;
        case kOpExecutionMode:
            if (code[i + 2] == kExecutionModeLocalSize) {
                out.localX = code[i + 3];
                out.localY = code[i + 4];
            }
            break;
        case kOpDecorate:
            if (code[i + 2] == kDecorationDescriptorSet)
                setWord[code[i + 1]] = i + 3;
            else if (code[i + 2] == kDecorationBinding)
                bindingWord[code[i + 1]] = i + 3;
            else if (code[i + 2] == kDecorationBufferBlock)
                bufferBlocks.insert(code[i + 1]);
            break;
        case kOpTypePointer: pointee[code[i + 1]] = code[i + 3]; break;
        case kOpTypeImage:
            opaque[code[i + 1]] =
                code[i + 7] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            break;
        case kOpTypeSampler: opaque[code[i + 1]] = VK_DESCRIPTOR_TYPE_SAMPLER; break;
        case kOpTypeSampledImage: opaque[code[i + 1]] = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
        case kOpTypeArray:
        case kOpTypeRuntimeArray: element[code[i + 1]] = code[i + 2]; break;
        case kOpVariable: variables.push_back({code[i + 2], code[i + 1], code[i + 3]}); break;
        default: break;
        }
        i += count;
    }
    out.stage = model == kModelVertex    ? VK_SHADER_STAGE_VERTEX_BIT
                : model == kModelCompute ? VK_SHADER_STAGE_COMPUTE_BIT
                                         : VK_SHADER_STAGE_FRAGMENT_BIT;

    for (const Variable& v : variables) {
        const auto set = setWord.find(v.id);
        const auto binding = bindingWord.find(v.id);
        if (set == setWord.end() || binding == bindingWord.end())
            continue;
        uint32_t type = pointee[v.pointer];
        while (element.count(type))
            type = element[type];
        VkDescriptorType kind;
        if (v.storage == kStorageStorageBuffer || (v.storage == kStorageUniform && bufferBlocks.count(type)))
            kind = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (v.storage == kStorageUniform)
            kind = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (v.storage == kStorageUniformConstant && opaque.count(type))
            kind = opaque[type];
        else
            continue;

        const uint32_t oldSet = code[set->second], oldBinding = code[binding->second];
        const std::string where = "set " + std::to_string(oldSet) + " binding " + std::to_string(oldBinding);
        if (kind == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            error = where + ": combined image samplers have no place in the layout";
            return false;
        }
        Binding b{kResourceSet, 0, kind};
        if (oldSet == kBufferSet && oldBinding < kBufferSlots)
            b.binding = oldBinding + (out.stage == VK_SHADER_STAGE_VERTEX_BIT ? kVertexBuffers : 0);
        else if (oldSet == kTextureSet && oldBinding < kTextureSlots)
            b.binding = kTextures + oldBinding;
        else if (oldSet == azl::kSamplerSet && oldBinding < kSamplerKinds && kind == VK_DESCRIPTOR_TYPE_SAMPLER)
            b = {vk::kSamplerSet, oldBinding, kind};
        else {
            error = where + " is outside the shader layout";
            return false;
        }
        code[set->second] = b.set;
        code[binding->second] = b.binding;
        out.bindings.push_back(b);
    }
    return true;
}

} // namespace azl::gpu::vk
