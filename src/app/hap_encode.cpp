#include "media/hap_encoder.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace {

constexpr const char* kUsage = R"(usage: nocturne-hap-encode --size WxH --fps N[/D] [options] OUT.mov < frames.rgba

Encodes raw RGBA frames (8 bits per channel, straight alpha, top row first) from stdin into a HAP movie, e.g.
  ffmpeg -i in.mp4 -an -f rawvideo -pix_fmt rgba - | nocturne-hap-encode --size 1920x1080 --fps 30 out.mov
(tools/transcode-media.sh does this with HAP=...).

  --codec C     hap (BC1, the default), hap-alpha (BC3 with alpha), hap-q (scaled YCoCg BC3, better colour) or
                hap-q-alpha (hap-q plus a BC4 alpha)
  --chunks N    Snappy chunks per texture, decoded in parallel (default 1)
)";

[[noreturn]] void usage(int code)
{
    std::fputs(kUsage, code ? stderr : stdout);
    std::exit(code);
}

} // namespace

int main(int argc, char** argv)
{
    int width = 0, height = 0, fpsNum = 0, fpsDen = 1, chunks = 1;
    std::string codecName = "hap";
    std::string out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--help" || arg == "-h")
            usage(0);
        if (arg == "--size" && hasValue) {
            if (std::sscanf(argv[++i], "%dx%d", &width, &height) != 2)
                usage(2);
        } else if (arg == "--fps" && hasValue) {
            if (std::sscanf(argv[++i], "%d/%d", &fpsNum, &fpsDen) < 1)
                usage(2);
        } else if (arg == "--codec" && hasValue) {
            codecName = argv[++i];
        } else if (arg == "--chunks" && hasValue) {
            chunks = std::atoi(argv[++i]);
        } else if (arg.rfind("--", 0) != 0 && out.empty()) {
            out = arg;
        } else {
            usage(2);
        }
    }
    const azl::HapCodec codec = azl::hapCodecNamed(codecName);
    if (out.empty() || width <= 0 || height <= 0 || fpsNum <= 0 || fpsDen <= 0 || chunks < 1 ||
        codec == azl::HapCodec::None)
        usage(2);
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    try {
        azl::HapEncoder encoder(out, codec, width, height, fpsNum, fpsDen, chunks);
        std::vector<uint8_t> buffer(size_t(1) << 20);
        for (size_t n; (n = std::fread(buffer.data(), 1, buffer.size(), stdin)) > 0;)
            encoder.feed(buffer.data(), n);
        encoder.finish();
        std::fprintf(stderr, "%d frames, %s\n", encoder.frames(), codecName.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nocturne-hap-encode: %s\n", e.what());
        return 1;
    }
    return 0;
}
