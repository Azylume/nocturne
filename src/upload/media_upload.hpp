#pragma once

#include "upload/s3.hpp"
#include "upload/upload_states.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace azl {

// ~/Library/Application Support/Nocturne/r2.json
std::filesystem::path defaultR2ConfigPath();
// ~/Library/Application Support/Nocturne/Uploads: converted files, resume state and run manifests.
std::filesystem::path defaultUploadsRoot();

// The Director's R2 settings: endpoint, bucket and access key id from `file` (JSON), each overridable by
// NOCTURNE_R2_ENDPOINT, NOCTURNE_R2_BUCKET, NOCTURNE_R2_ACCESS_KEY_ID. The secret comes from
// NOCTURNE_R2_SECRET_ACCESS_KEY, else the file's "secret_access_key" (only if the file is private to its owner), else
// the login keychain (generic password, service `keychainService`, account = the access key id). `env` replaces the
// NOCTURNE_R2 prefix of the variables. Throws std::runtime_error with what to set when anything is missing.
S3Config loadR2Config(const std::filesystem::path& file, const std::string& keychainService = "nocturne-r2",
                      const std::string& env = "NOCTURNE_R2");

struct ConvertOptions {
    int crf = 18;
    std::string preset = "slow";
    // "h264", or a HAP codec ("hap", "hap-alpha", "hap-q", "hap-q-alpha"): ffmpeg decodes, our encoder writes the
    // HAP movie, every frame a keyframe; a still becomes a 4 s loop with its alpha kept.
    std::string codec = "h264";
    int hapChunks = 1;
};

struct Conversion {
    std::filesystem::path file;
    std::string kind; // "encoded", "still", "remuxed" (already H.264 with a 1 s GOP), "hap", "cached"
};

// The source in the playback format (tools/transcode-media.sh): H.264 up to 1080p, a keyframe every second, no audio;
// a still becomes a 4 s loop. With a HAP codec, a HAP movie up to 1080p instead. Written once into `outDir` under a name carrying the source's hash, so a rerun returns
// the earlier result ("cached"). Runs ffprobe and ffmpeg (at nice 15). Throws std::runtime_error.
Conversion convertForPlayback(const std::filesystem::path& source, const std::filesystem::path& outDir,
                              const ConvertOptions& options = {});

struct UploadOptions {
    // A larger file goes up in parts of this size.
    uint64_t partSize = 16ull << 20;
    // Multipart uploads in flight, to resume after a drop.
    std::filesystem::path stateDir = defaultUploadsRoot() / "pending";
    // Per request, on a dropped connection or a 5xx reply.
    int attempts = 3;
    // Bytes in the bucket so far, as the upload starts and after each part.
    std::function<void(uint64_t bytes, uint64_t size)> progress;
};

struct UploadResult {
    std::string state; // "uploaded" or "present" (already in the bucket)
    int parts = 0;
    int resumedParts = 0; // parts an interrupted earlier run had uploaded already
};

UploadResult uploadObject(S3Client& client, const std::filesystem::path& file, const std::string& key,
                          const UploadOptions& options);

struct RunOptions {
    std::filesystem::path convertDir = defaultUploadsRoot() / "converted";
    std::filesystem::path manifest;
    ConvertOptions convert;
    UploadOptions upload;
    // Each uploaded hash's state, as UploadStates keeps it; empty for none.
    std::filesystem::path states = defaultUploadStatesFile();
};

// Converts each source and uploads it under its content hash (the Media Library's sha256File of the converted file);
// with no client it only converts. Each upload's state goes to options.states as it goes. Writes the run's manifest to options.manifest, also when files fail, and returns
// it. Rejected credentials stop the run with a std::runtime_error saying so; other failures are recorded per file.
nlohmann::json runUpload(S3Client* client, const std::vector<std::filesystem::path>& sources,
                         const RunOptions& options);

// Files as given, and the media files directly inside each directory given, by name.
std::vector<std::filesystem::path> mediaSources(const std::vector<std::filesystem::path>& arguments);
// defaultUploadsRoot()/manifests/TIME.json, TIME in UTC.
std::filesystem::path newManifestPath();
// A manifest file's line: status, source, hash and error.
std::string manifestLine(const nlohmann::json& file);

} // namespace azl
