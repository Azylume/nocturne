#include "upload/media_upload.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kUsage = R"(usage: nocturne-upload [options] FILE|DIR...

Converts media into the playback format (H.264 up to 1080p, a keyframe every second, no audio; a still becomes a 4 s
loop) and uploads each converted file to the private R2 bucket under its content hash, the Media Library's hash. An
object already in the bucket is not uploaded again, a file converted before is not converted again, and a large file
goes up in parts: a run cut off midway resumes where it stopped when started again. Each run writes a manifest, and
each file's upload state goes to ~/Library/Application Support/Nocturne/Uploads/states.json for the library's badge.

R2 settings come from the config file, JSON with "endpoint" (https://ACCOUNT.r2.cloudflarestorage.com), "bucket" and
"access_key_id", or from NOCTURNE_R2_ENDPOINT, NOCTURNE_R2_BUCKET and NOCTURNE_R2_ACCESS_KEY_ID. The secret is read
from the login keychain (service nocturne-r2, account = the access key id) or NOCTURNE_R2_SECRET_ACCESS_KEY.

  --config FILE        R2 settings (default: ~/Library/Application Support/Nocturne/r2.json)
  --convert-dir DIR    converted files (default: ~/Library/Application Support/Nocturne/Uploads/converted)
  --manifest FILE      the run's manifest (default: ~/Library/Application Support/Nocturne/Uploads/manifests/TIME.json)
  --part-size MB       multipart part size, at least 5 (default 16)
  --crf N, --preset P  x264 quality and speed (default 18, slow)
  --codec C            h264 (the default), or HAP: hap, hap-alpha, hap-q or hap-q-alpha (a .mov, every frame a
                       keyframe, alpha kept for the alpha variants; larger files, less decode work)
  --hap-chunks N       Snappy chunks per HAP texture, decoded in parallel (default 1)
  --convert-only       convert and hash; upload nothing and need no credentials
)";

[[noreturn]] void usage(int code)
{
    std::fputs(kUsage, code ? stderr : stdout);
    std::exit(code);
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<fs::path> arguments;
    fs::path configPath = azl::defaultR2ConfigPath();
    azl::RunOptions options;
    bool convertOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&] {
            if (i + 1 >= argc)
                usage(2);
            return std::string(argv[++i]);
        };
        if (arg == "--config") configPath = value();
        else if (arg == "--convert-dir") options.convertDir = value();
        else if (arg == "--manifest") options.manifest = value();
        else if (arg == "--part-size") options.upload.partSize = uint64_t(std::stoul(value())) << 20;
        else if (arg == "--crf") options.convert.crf = std::stoi(value());
        else if (arg == "--preset") options.convert.preset = value();
        else if (arg == "--codec") options.convert.codec = value();
        else if (arg == "--hap-chunks") options.convert.hapChunks = std::stoi(value());
        else if (arg == "--convert-only") convertOnly = true;
        else if (arg == "--help" || arg == "-h") usage(0);
        else if (arg.rfind("--", 0) == 0) usage(2);
        else arguments.push_back(arg);
    }
    if (arguments.empty() || options.upload.partSize < (5u << 20))
        usage(2);
    if (options.manifest.empty())
        options.manifest = azl::newManifestPath();

    try {
        std::optional<azl::S3Client> client;
        if (!convertOnly)
            client.emplace(azl::loadR2Config(configPath));
        const nlohmann::json manifest = azl::runUpload(client ? &*client : nullptr, azl::mediaSources(arguments), options);
        int failed = 0;
        for (const nlohmann::json& file : manifest["files"]) {
            failed += file["status"] == "failed";
            std::puts(azl::manifestLine(file).c_str());
        }
        std::printf("manifest: %s\n", options.manifest.c_str());
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nocturne-upload: %s\n", e.what());
        return 1;
    }
}
