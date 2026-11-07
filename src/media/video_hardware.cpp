#include "media/video_hardware.hpp"

#if defined(_WIN32)

#include <windows.h>

#include <d3d11_4.h>
#include <d3dcommon.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace azl {

namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kCopyTimeoutMs = 2000;
// Large enough for any decoder to accept; only proves the Vulkan device can import the planes.
constexpr UINT kProbeSize = 64;

// One full-screen triangle that copies a plane of the NV12 picture, texel for texel, into a target of the plane's size.
// The planes go into textures of their own because Vulkan and D3D11 disagree on where an NV12 image keeps its chroma.
constexpr const char* kShader = R"(
Texture2D<float4> plane : register(t0);
float4 fullScreen(uint id : SV_VertexID) : SV_Position
{
    return float4(float(id & 2) * 2.0 - 1.0, 1.0 - float((id << 1) & 2) * 2.0, 0.0, 1.0);
}
float4 copyPlane(float4 position : SV_Position) : SV_Target
{
    return plane.Load(int3(position.xy, 0));
}
)";

constexpr DXGI_FORMAT kPlaneFormats[2] = {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM};

// The D3D11 device of one adapter, wrapped for FFmpeg's D3D11VA decoders, for every source and the whole process.
struct SharedDevice {
    ~SharedDevice() { av_buffer_unref(&av); }

    ComPtr<ID3D11Device5> device;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11RasterizerState> rasterizer;
    AVBufferRef* av = nullptr;
    AVD3D11VADeviceContext* hardware = nullptr;
};

bool makeShaders(SharedDevice& shared)
{
    using Compile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);
    const HMODULE library = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile =
        library ? reinterpret_cast<Compile>(reinterpret_cast<void*>(GetProcAddress(library, "D3DCompile"))) : nullptr;
    ComPtr<ID3DBlob> vertex, pixel;
    if (!compile ||
        FAILED(compile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "fullScreen", "vs_5_0", 0, 0, &vertex,
                       nullptr)) ||
        FAILED(compile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "copyPlane", "ps_5_0", 0, 0, &pixel,
                       nullptr)))
        return false;
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    return SUCCEEDED(shared.device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr,
                                                       &shared.vertex)) &&
           SUCCEEDED(shared.device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr,
                                                      &shared.pixel)) &&
           SUCCEEDED(shared.device->CreateRasterizerState(&rasterizer, &shared.rasterizer));
}

// A decoded picture's landing place: an NV12 texture a decoder's picture is copied into, and the two plane textures
// drawn from it, which the Vulkan device has imported.
struct Stage {
    ComPtr<ID3D11Texture2D> nv12;
    ComPtr<ID3D11ShaderResourceView> views[2];
    ComPtr<ID3D11RenderTargetView> targets[2];
    ComPtr<ID3D11Texture2D> planes[2];
    gpu::SharedPicture picture;
};

// Makes plane texture `index` of `stage` and its shared handle; false when D3D11 refuses.
bool makePlane(SharedDevice& shared, Stage& stage, int index, UINT width, UINT height, HANDLE& handle)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = index ? width / 2 : width;
    desc.Height = index ? height / 2 : height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kPlaneFormats[index];
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = kPlaneFormats[index];
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    ComPtr<IDXGIResource1> resource;
    return SUCCEEDED(shared.device->CreateTexture2D(&desc, nullptr, &stage.planes[index])) &&
           SUCCEEDED(shared.device->CreateRenderTargetView(stage.planes[index].Get(), nullptr, &stage.targets[index])) &&
           SUCCEEDED(shared.device->CreateShaderResourceView(stage.nv12.Get(), &view, &stage.views[index])) &&
           SUCCEEDED(stage.planes[index].As(&resource)) &&
           SUCCEEDED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                  nullptr, &handle));
}

// Without a picture when D3D11 or the Vulkan device refuses. `width` and `height` are even.
Stage makeStage(SharedDevice& shared, const gpu::Device& device, UINT width, UINT height)
{
    Stage stage;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HANDLE handles[2] = {};
    if (SUCCEEDED(shared.device->CreateTexture2D(&desc, nullptr, &stage.nv12)) &&
        makePlane(shared, stage, 0, width, height, handles[0]) &&
        makePlane(shared, stage, 1, width, height, handles[1]))
        stage.picture = device.importSharedPicture(handles[0], handles[1], width, height);
    for (HANDLE handle : handles)
        if (handle)
            CloseHandle(handle);
    return stage;
}

std::shared_ptr<SharedDevice> openSharedDevice(const gpu::Device& device)
{
    LUID luid;
    const uint64_t adapterId = device.sharingAdapter();
    std::memcpy(&luid, &adapterId, sizeof(luid));
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D11Device> d3d;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)))
        return nullptr;
    auto shared = std::make_shared<SharedDevice>();
    ComPtr<ID3D11Multithread> multithread;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(d3d.As(&shared->device)) || FAILED(d3d.As(&multithread)))
        return nullptr;
    multithread->SetMultithreadProtected(TRUE);
    d3d->GetImmediateContext(&context);
    if (FAILED(context.As(&shared->context)) || !makeShaders(*shared))
        return nullptr;
    shared->av = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!shared->av)
        return nullptr;
    shared->hardware = static_cast<AVD3D11VADeviceContext*>(
        reinterpret_cast<AVHWDeviceContext*>(shared->av->data)->hwctx);
    shared->hardware->device = d3d.Detach();
    if (av_hwdevice_ctx_init(shared->av) < 0)
        return nullptr;
    return makeStage(*shared, device, kProbeSize, kProbeSize).picture ? shared : nullptr;
}

// Opened once per adapter, a failure too: a machine that cannot decode on its GPU does not retry per clip.
std::shared_ptr<SharedDevice> sharedDevice(const gpu::Device& device)
{
    static std::mutex mutex;
    static std::map<uint64_t, std::shared_ptr<SharedDevice>> devices;
    const uint64_t adapter = device.sharingAdapter();
    if (!adapter)
        return nullptr;
    std::lock_guard lock(mutex);
    auto it = devices.find(adapter);
    if (it != devices.end())
        return it->second;
    std::shared_ptr<SharedDevice> shared = openSharedDevice(device);
    std::fprintf(stderr, shared ? "nocturne: video decodes on the GPU through D3D11VA, shared with Vulkan\n"
                                : "nocturne: no GPU video decode shared with Vulkan; video decodes on the CPU\n");
    devices.emplace(adapter, shared);
    return shared;
}

bool softwareOnly()
{
    const char* value = std::getenv("NOCTURNE_SOFTWARE_DECODE");
    return value && *value && *value != '0';
}

} // namespace

struct GpuDecode::Impl {
    gpu::Device device;
    std::shared_ptr<SharedDevice> shared;
    ComPtr<ID3D11Fence> fence;
    HANDLE done = nullptr;
    uint64_t fenceValue = 0;
    Stage stage;
    UINT width = 0;
    UINT height = 0;

    ~Impl()
    {
        if (done)
            CloseHandle(done);
    }

    bool prepare(UINT w, UINT h)
    {
        if (stage.picture && w == width && h == height)
            return true;
        stage = makeStage(*shared, device, w, h);
        width = w;
        height = h;
        return bool(stage.picture);
    }

    void lock() const { shared->hardware->lock(shared->hardware->lock_ctx); }
    void unlock() const { shared->hardware->unlock(shared->hardware->lock_ctx); }

    // Draws the stage's NV12 texture into its plane textures. Under lock().
    void drawPlanes() const
    {
        ID3D11DeviceContext4* context = shared->context.Get();
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(shared->vertex.Get(), nullptr, 0);
        context->PSSetShader(shared->pixel.Get(), nullptr, 0);
        context->RSSetState(shared->rasterizer.Get());
        for (int i = 0; i < 2; ++i) {
            const D3D11_VIEWPORT viewport{0, 0, float(i ? width / 2 : width), float(i ? height / 2 : height), 0, 1};
            context->RSSetViewports(1, &viewport);
            ID3D11RenderTargetView* target = stage.targets[i].Get();
            context->OMSetRenderTargets(1, &target, nullptr);
            ID3D11ShaderResourceView* view = stage.views[i].Get();
            context->PSSetShaderResources(0, 1, &view);
            context->Draw(3, 0);
        }
        ID3D11ShaderResourceView* none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    // The decoded picture drawn into the stage's plane textures, finished when this returns.
    bool copyToStage(const AVFrame& frame)
    {
        auto* source = reinterpret_cast<ID3D11Texture2D*>(frame.data[0]);
        const UINT slice = UINT(reinterpret_cast<intptr_t>(frame.data[1]));
        const D3D11_BOX box{0, 0, 0, UINT(frame.width), UINT(frame.height), 1};
        const uint64_t value = ++fenceValue;
        lock();
        shared->context->CopySubresourceRegion(stage.nv12.Get(), 0, 0, 0, 0, source, slice, &box);
        drawPlanes();
        const bool signalled = SUCCEEDED(shared->context->Signal(fence.Get(), value)) &&
                               SUCCEEDED(fence->SetEventOnCompletion(value, done));
        unlock();
        return signalled && WaitForSingleObject(done, kCopyTimeoutMs) == WAIT_OBJECT_0;
    }
};

GpuDecode::GpuDecode(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GpuDecode::~GpuDecode() = default;

std::unique_ptr<GpuDecode> GpuDecode::open(const gpu::Device& device)
{
    if (softwareOnly() || !device)
        return nullptr;
    std::shared_ptr<SharedDevice> shared = sharedDevice(device);
    if (!shared)
        return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->device = device;
    impl->shared = std::move(shared);
    impl->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl->done || FAILED(impl->shared->device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->fence))))
        return nullptr;
    return std::unique_ptr<GpuDecode>(new GpuDecode(std::move(impl)));
}

AVBufferRef* GpuDecode::device() const
{
    return impl_->shared->av;
}

bool GpuDecode::takes(const AVFrame& frame) const
{
    if (frame.format != AV_PIX_FMT_D3D11 || !frame.hw_frames_ctx || (frame.width | frame.height) & 1)
        return false;
    return reinterpret_cast<const AVHWFramesContext*>(frame.hw_frames_ctx->data)->sw_format == AV_PIX_FMT_NV12;
}

std::array<gpu::Texture, 2> GpuDecode::textures(const AVFrame& frame)
{
    Impl& d = *impl_;
    if (!takes(frame) || !d.prepare(UINT(frame.width), UINT(frame.height)))
        return {};
    const uint32_t width = uint32_t(frame.width), height = uint32_t(frame.height);
    std::array<gpu::Texture, 2> out{d.device.makeTexture({.format = gpu::Format::R8, .width = width, .height = height}),
                                    d.device.makeTexture(
                                        {.format = gpu::Format::RG8, .width = width / 2, .height = height / 2})};
    if (!out[0] || !out[1] || !d.copyToStage(frame) || !d.device.copyPicture(d.stage.picture, out[0], out[1]))
        return {};
    return out;
}

} // namespace azl

#else

namespace azl {

struct GpuDecode::Impl {};

GpuDecode::GpuDecode(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

GpuDecode::~GpuDecode() = default;

std::unique_ptr<GpuDecode> GpuDecode::open(const gpu::Device&)
{
    return nullptr;
}

AVBufferRef* GpuDecode::device() const
{
    return nullptr;
}

bool GpuDecode::takes(const AVFrame&) const
{
    return false;
}

std::array<gpu::Texture, 2> GpuDecode::textures(const AVFrame&)
{
    return {};
}

} // namespace azl

#endif
