#include "media/video_hardware.hpp"

#include "gpu/gpu_vulkan.hpp"

extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

namespace azl {

namespace {

bool softwareOnly()
{
    const char* software = std::getenv("NOCTURNE_SOFTWARE_DECODE");
    return software && *software && *software != '0';
}

// The context FFmpeg decodes with, built over the device's handles and the queue families it reserved. Null on failure.
AVBufferRef* makeContext(const gpu::vulkan::VideoDecodeDevice& video)
{
    AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!ref)
        return nullptr;
    auto* context = reinterpret_cast<AVHWDeviceContext*>(ref->data);
    auto* vulkan = static_cast<AVVulkanDeviceContext*>(context->hwctx);
    vulkan->get_proc_addr = video.getInstanceProcAddr;
    vulkan->inst = video.instance;
    vulkan->phys_dev = video.physicalDevice;
    vulkan->act_dev = video.logicalDevice;
    vulkan->device_features = video.features;
    vulkan->enabled_dev_extensions = video.extensions.data();
    vulkan->nb_enabled_dev_extensions = int(video.extensions.size());
    for (const gpu::vulkan::VideoFamily& family : video.families) {
        vulkan->qf[vulkan->nb_qf++] = {int(family.index), int(family.queues), VkQueueFlagBits(family.flags),
                                       VkVideoCodecOperationFlagBitsKHR(0)};
    }
    if (const int error = av_hwdevice_ctx_init(ref); error < 0) {
        std::fprintf(stderr, "nocturne: FFmpeg cannot use the Vulkan device (error %d); video decodes on the CPU\n",
                     error);
        av_buffer_unref(&ref);
    }
    return ref;
}

} // namespace

std::shared_ptr<GpuDecode> GpuDecode::open(const gpu::Device& device)
{
    static std::mutex mutex;
    static std::map<VkDevice, std::weak_ptr<GpuDecode>> shared;
    if (!device || softwareOnly())
        return nullptr;
    const gpu::vulkan::VideoDecodeDevice* video = gpu::vulkan::videoDecode(device);
    std::lock_guard lock(mutex);
    if (!video) {
        static bool logged = false;
        if (!std::exchange(logged, true))
            std::fprintf(stderr, "nocturne: the GPU has no Vulkan Video decode or no spare queue; video decodes on "
                                 "the CPU\n");
        return nullptr;
    }
    std::weak_ptr<GpuDecode>& slot = shared[video->logicalDevice];
    if (auto existing = slot.lock())
        return existing;
    AVBufferRef* context = makeContext(*video);
    if (!context)
        return nullptr;
    std::shared_ptr<GpuDecode> decode(new GpuDecode(device, context));
    std::fprintf(stderr, "nocturne: video decodes on the GPU through Vulkan Video (H.264 and HEVC, 8-bit up to the "
                         "driver's limits); anything else on the CPU\n");
    slot = decode;
    return decode;
}

GpuDecode::~GpuDecode()
{
    av_buffer_unref(&device_);
}

bool GpuDecode::takes(const AVFrame& frame, int width, int height) const
{
    if (frame.format != AV_PIX_FMT_VULKAN || !frame.hw_frames_ctx || !frame.data[0])
        return false;
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data);
    const auto* image = reinterpret_cast<const AVVkFrame*>(frame.data[0]);
    return frames->sw_format == AV_PIX_FMT_NV12 && image->img[1] == VK_NULL_HANDLE &&
           image->queue_family[0] == VK_QUEUE_FAMILY_IGNORED && frame.crop_left == 0 && frame.crop_top == 0 &&
           frame.width == width && frame.height == height && !((width | height) & 1);
}

std::array<gpu::Texture, 2> GpuDecode::textures(const AVFrame& frame) const
{
    const auto w = uint32_t(frame.width), h = uint32_t(frame.height);
    std::array<gpu::Texture, 2> out{
        gpu_.makeTexture({.format = gpu::Format::R8, .width = w, .height = h, .memory = gpu::Memory::Upload}),
        gpu_.makeTexture({.format = gpu::Format::RG8, .width = w / 2, .height = h / 2, .memory = gpu::Memory::Upload})};
    if (!out[0] || !out[1])
        return {};
    const auto* frames = reinterpret_cast<AVHWFramesContext*>(frame.hw_frames_ctx->data);
    const auto* frameContext = static_cast<AVVulkanFramesContext*>(frames->hwctx);
    auto* image = reinterpret_cast<AVVkFrame*>(frame.data[0]);
    frameContext->lock_frame(const_cast<AVHWFramesContext*>(frames), image);
    const gpu::vulkan::DecodedImage source{image->img[0],       image->layout[0], image->access[0],
                                           image->sem[0],       image->sem_value[0], w, h};
    const bool copied = gpu::vulkan::copyDecodedPlanes(gpu_, source, out[0], out[1], [&](bool submitted) {
        if (submitted) {
            image->layout[0] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            image->access[0] = VK_ACCESS_2_TRANSFER_READ_BIT;
            ++image->sem_value[0];
        }
        frameContext->unlock_frame(const_cast<AVHWFramesContext*>(frames), image);
    });
    return copied ? out : std::array<gpu::Texture, 2>{};
}

} // namespace azl
