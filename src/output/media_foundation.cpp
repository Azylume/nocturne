#include "output/media_foundation.hpp"

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#endif

namespace azl::mf {

namespace {

#ifdef _WIN32
bool listsHardwareEncoder(const GUID& subtype)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, subtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT result = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                     nullptr, &output, &activates, &count);
    for (UINT32 i = 0; i < count; ++i)
        activates[i]->Release();
    CoTaskMemFree(activates);
    if (SUCCEEDED(com))
        CoUninitialize();
    return SUCCEEDED(result) && count > 0;
}
#endif

} // namespace

bool hasHardwareEncoder([[maybe_unused]] AVCodecID codec)
{
#ifdef _WIN32
    switch (codec) {
    case AV_CODEC_ID_H264: {
        static const bool listed = listsHardwareEncoder(MFVideoFormat_H264);
        return listed;
    }
    case AV_CODEC_ID_HEVC: {
        static const bool listed = listsHardwareEncoder(MFVideoFormat_HEVC);
        return listed;
    }
    case AV_CODEC_ID_AV1: {
        static const bool listed = listsHardwareEncoder(MFVideoFormat_AV1);
        return listed;
    }
    default:
        return false;
    }
#else
    return false;
#endif
}

} // namespace azl::mf
