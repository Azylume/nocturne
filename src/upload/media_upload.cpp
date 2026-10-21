#include "upload/media_upload.hpp"

#include "document/composition_file.hpp"
#include "media/hap_encoder.hpp"
#include "media/media_library.hpp"

#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

extern char** environ;

namespace fs = std::filesystem;

namespace azl {

namespace {

constexpr const char* kContentType = "video/mp4";

// Runs a command and returns its standard output; throws when it cannot start or exits non-zero.
// Runs the tool and returns its output; with a `sink`, its output goes there as it comes instead.
std::string runTool(const std::vector<std::string>& args, bool quiet = false,
                    const std::function<void(const char*, size_t)>& sink = {})
{
    int out[2];
    if (pipe(out) != 0)
        throw std::runtime_error("pipe: " + std::string(std::strerror(errno)));
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    posix_spawn_file_actions_addclose(&actions, out[1]);
    if (quiet)
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::vector<char*> argv;
    for (const std::string& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    const std::string name = args[0] == "nice" ? args[3] : args[0];
    if (spawned != 0) {
        close(out[0]);
        throw std::runtime_error("cannot run " + name + ": " + std::strerror(spawned));
    }
    std::string output;
    char buffer[4096];
    try {
        for (ssize_t n; (n = read(out[0], buffer, sizeof buffer)) > 0;)
            sink ? sink(buffer, size_t(n)) : void(output.append(buffer, size_t(n)));
    } catch (...) {
        close(out[0]);
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        throw;
    }
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw std::runtime_error(name + " failed (exit " + std::to_string(WEXITSTATUS(status)) + ")");
    return output;
}

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        if (!line.empty())
            out.push_back(line);
    return out;
}

std::vector<std::string> fields(const std::string& line)
{
    std::vector<std::string> out;
    std::istringstream in(line);
    for (std::string field; std::getline(in, field, ',');)
        out.push_back(field);
    return out;
}

std::string utcText(const char* format)
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char text[32];
    std::strftime(text, sizeof text, format, &tm);
    return text;
}

struct Probe {
    std::string codec;
    int width = 0, height = 0;
    double fps = 0;
    int fpsNum = 0, fpsDen = 1;
    double duration = 0;
};

Probe probe(const fs::path& file)
{
    Probe p;
    for (const std::string& line :
         lines(runTool({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "stream=codec_name,width,height,avg_frame_rate:format=duration", "-of", "csv=p=0", file}))) {
        const auto f = fields(line);
        if (f.size() == 1) {
            p.duration = std::atof(f[0].c_str());
            continue;
        }
        if (f.size() < 4)
            continue;
        p.codec = f[0];
        p.width = std::atoi(f[1].c_str());
        p.height = std::atoi(f[2].c_str());
        const int den = std::atoi(f[3].substr(f[3].find('/') + 1).c_str());
        p.fps = den ? std::atof(f[3].c_str()) / den : 0;
        p.fpsNum = std::atoi(f[3].c_str());
        p.fpsDen = den;
    }
    if (p.codec.empty())
        throw std::runtime_error(file.string() + " has no video stream");
    return p;
}

// Whether no stretch between keyframes, or from the last one to the end, is longer than a second.
bool hasOneSecondGop(const fs::path& file, double duration)
{
    std::vector<double> keyframes;
    for (const std::string& line : lines(runTool({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                                  "packet=pts_time,flags", "-of", "csv=p=0", file}))) {
        const auto f = fields(line);
        if (f.size() >= 2 && !f[1].empty() && f[1][0] == 'K' && f[0] != "N/A")
            keyframes.push_back(std::atof(f[0].c_str()));
    }
    if (keyframes.empty() || duration <= 0)
        return false;
    std::sort(keyframes.begin(), keyframes.end());
    keyframes.push_back(duration);
    for (size_t i = 1; i < keyframes.size(); ++i)
        if (keyframes[i] - keyframes[i - 1] > 1.05)
            return false;
    return true;
}

// The ffmpeg arguments match tools/transcode-media.sh.
std::vector<std::string> ffmpegCommand(const std::vector<std::string>& args, const fs::path& target)
{
    std::vector<std::string> command{"nice", "-n", "15", "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                                     "-y"};
    command.insert(command.end(), args.begin(), args.end());
    command.insert(command.end(), {"-map_metadata", "-1", "-tag:v", "avc1", "-movflags", "+faststart", target});
    return command;
}

std::vector<std::string> encodeArgs(const fs::path& source, double fps, const ConvertOptions& options)
{
    const std::string gop = std::to_string(std::max(1L, std::lround(fps)));
    return {"-i", source, "-an", "-vf",
            "scale=w='min(1920,iw)':h='min(1080,ih)':force_original_aspect_ratio=decrease,format=yuv420p", "-c:v",
            "libx264", "-preset", options.preset, "-crf", std::to_string(options.crf), "-profile:v", "high", "-g", gop,
            "-keyint_min", gop, "-sc_threshold", "0"};
}

std::vector<std::string> stillArgs(const fs::path& source, const ConvertOptions& options)
{
    return {"-loop", "1", "-t", "4", "-framerate", "30", "-i", source, "-an", "-filter_complex",
            "color=black:s=1920x1080:r=30:d=4[bg];[0:v]scale=1920:1080:force_original_aspect_ratio=decrease[fg];[bg]["
            "fg]overlay=(W-w)/2:(H-h)/2:format=auto,format=yuv420p",
            "-c:v", "libx264", "-preset", options.preset, "-crf", std::to_string(options.crf), "-profile:v", "high", "-g",
            "30", "-keyint_min", "30", "-sc_threshold", "0"};
}

std::vector<std::string> remuxArgs(const fs::path& source)
{
    return {"-i", source, "-map", "0:v:0", "-an", "-c:v", "copy"};
}

bool isStill(const std::string& codec)
{
    return codec == "png" || codec == "mjpeg" || codec == "tiff" || codec == "bmp" || codec == "webp";
}

// Decodes the source with ffmpeg to raw RGBA, at most 1080p, and encodes it as a HAP movie.
void convertToHap(const fs::path& source, const Probe& p, const fs::path& target, const ConvertOptions& options)
{
    const azl::HapCodec codec = hapCodecNamed(options.codec);
    if (codec == HapCodec::None)
        throw std::runtime_error("unknown codec " + options.codec);
    const double scale = std::min({1.0, 1920.0 / std::max(1, p.width), 1080.0 / std::max(1, p.height)});
    const int width = std::max(2, int(std::lround(p.width * scale / 2)) * 2);
    const int height = std::max(2, int(std::lround(p.height * scale / 2)) * 2);
    const bool still = isStill(p.codec);
    const int fpsNum = still || p.fpsNum <= 0 ? 30 : p.fpsNum;
    const int fpsDen = still || p.fpsNum <= 0 ? 1 : std::max(1, p.fpsDen);
    std::vector<std::string> command{"nice", "-n", "15", "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error"};
    if (still)
        command.insert(command.end(), {"-loop", "1", "-t", "4", "-framerate", "30"});
    command.insert(command.end(), {"-i", source.string(), "-an", "-vf",
                                   "scale=" + std::to_string(width) + ":" + std::to_string(height), "-f", "rawvideo",
                                   "-pix_fmt", "rgba", "-"});
    HapEncoder encoder(target, codec, width, height, fpsNum, fpsDen, options.hapChunks);
    runTool(command, false, [&](const char* data, size_t size) {
        encoder.feed(reinterpret_cast<const uint8_t*>(data), size);
    });
    encoder.finish();
}

std::string readRange(const fs::path& file, uint64_t offset, uint64_t size)
{
    std::ifstream in(file, std::ios::binary);
    std::string data(size, '\0');
    in.seekg(std::streamoff(offset));
    in.read(data.data(), std::streamsize(size));
    if (uint64_t(in.gcount()) != size)
        throw std::runtime_error("cannot read " + file.string());
    return data;
}

template <class Call>
auto retrying(int attempts, Call&& call)
{
    for (int attempt = 1;; ++attempt) {
        try {
            return call();
        } catch (const TransportError&) {
            if (attempt >= attempts)
                throw;
        } catch (const S3Error& e) {
            if (e.status < 500 || attempt >= attempts)
                throw;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1 << std::min(attempt - 1, 4)));
    }
}

void report(const UploadOptions& options, uint64_t bytes, uint64_t size)
{
    if (options.progress)
        options.progress(bytes, size);
}

std::optional<nlohmann::json> readJson(const fs::path& file)
{
    std::ifstream in(file);
    if (!in)
        return std::nullopt;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    return j.is_discarded() ? std::nullopt : std::optional(j);
}

UploadResult uploadParts(S3Client& client, const fs::path& file, const std::string& key, uint64_t size,
                         const UploadOptions& options)
{
    const uint64_t partSize = options.partSize;
    const int count = int((size + partSize - 1) / partSize);
    auto sizeOf = [&](int number) { return std::min(partSize, size - uint64_t(number - 1) * partSize); };
    const fs::path statePath = options.stateDir / (key + ".json");
    const nlohmann::json identity = {
        {"endpoint", client.config().endpoint}, {"bucket", client.config().bucket}, {"size", size}, {"part_size", partSize}};

    std::string uploadId;
    std::map<int, S3Part> done;
    if (const auto state = readJson(statePath); state && state->value("for", nlohmann::json()) == identity) {
        uploadId = state->value("upload_id", "");
        const auto listed = retrying(options.attempts, [&] { return client.listParts(key, uploadId); });
        if (!listed)
            uploadId.clear();
        for (const S3Part& part : listed.value_or(std::vector<S3Part>{}))
            if (part.number >= 1 && part.number <= count && part.size == sizeOf(part.number))
                done[part.number] = part;
    }
    if (uploadId.empty()) {
        uploadId = retrying(options.attempts, [&] { return client.createMultipart(key, kContentType); });
        fs::create_directories(options.stateDir);
        writeFileAtomically(statePath, nlohmann::json{{"for", identity}, {"upload_id", uploadId}}.dump(2) + "\n");
    }

    const UploadResult result{"uploaded", count, int(done.size())};
    uint64_t sent = 0;
    for (const auto& [number, part] : done)
        sent += part.size;
    report(options, sent, size);
    for (int number = 1; number <= count; ++number) {
        if (done.count(number))
            continue;
        const std::string body = readRange(file, uint64_t(number - 1) * partSize, sizeOf(number));
        const std::string etag =
            retrying(options.attempts, [&] { return client.uploadPart(key, uploadId, number, body); });
        done[number] = {number, etag, body.size()};
        report(options, sent += body.size(), size);
    }
    std::vector<S3Part> parts;
    for (const auto& [number, part] : done)
        parts.push_back(part);
    retrying(options.attempts, [&] { client.completeMultipart(key, uploadId, parts); });
    fs::remove(statePath);
    return result;
}

std::string setting(const nlohmann::json& file, const char* key, const std::string& env)
{
    const char* value = std::getenv(env.c_str());
    return value && *value ? value : file.value(key, "");
}

std::string keychainSecret(const std::string& service, const std::string& account)
{
    std::string secret =
        runTool({"/usr/bin/security", "find-generic-password", "-s", service, "-a", account, "-w"}, true);
    while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r'))
        secret.pop_back();
    return secret;
}

std::string credentialsRefused(const S3Config& config, const S3Error& error)
{
    return "R2 refused the credentials for bucket " + config.bucket + " at " + config.endpoint + " (" +
           (error.code.empty() ? "HTTP " + std::to_string(error.status) : error.code) +
           "): check the access key id and secret, and that the token may write to this bucket";
}

} // namespace

fs::path defaultR2ConfigPath()
{
    return MediaLibrary::defaultRoot().parent_path() / "r2.json";
}

fs::path defaultUploadsRoot()
{
    return MediaLibrary::defaultRoot().parent_path() / "Uploads";
}

S3Config loadR2Config(const fs::path& path, const std::string& keychainService, const std::string& env)
{
    nlohmann::json file = nlohmann::json::object();
    if (fs::exists(path)) {
        const auto parsed = readJson(path);
        if (!parsed || !parsed->is_object())
            throw std::runtime_error(path.string() + " is not a JSON object");
        file = *parsed;
    }
    S3Config config;
    config.endpoint = setting(file, "endpoint", env + "_ENDPOINT");
    config.bucket = setting(file, "bucket", env + "_BUCKET");
    config.accessKeyId = setting(file, "access_key_id", env + "_ACCESS_KEY_ID");
    config.region = file.value("region", "auto");
    std::string missing;
    for (const auto& [value, name] : {std::pair{&config.endpoint, "endpoint"}, std::pair{&config.bucket, "bucket"},
                                      std::pair{&config.accessKeyId, "access_key_id"}})
        if (value->empty())
            missing += (missing.empty() ? "" : ", ") + std::string(name);
    if (!missing.empty())
        throw std::runtime_error("R2 settings missing (" + missing + "): set them in " + path.string() +
                                 " or as " + env + "_ENDPOINT, " + env + "_BUCKET, " + env + "_ACCESS_KEY_ID");

    if (const char* secret = std::getenv((env + "_SECRET_ACCESS_KEY").c_str()); secret && *secret) {
        config.secretAccessKey = secret;
        return config;
    }
    if (file.contains("secret_access_key")) {
        const auto shared = fs::perms::group_all | fs::perms::others_all;
        if ((fs::status(path).permissions() & shared) != fs::perms::none)
            throw std::runtime_error(path.string() + " holds the R2 secret but others can read it: chmod 600 it, or " +
                                     "keep the secret in the keychain");
        config.secretAccessKey = file.value("secret_access_key", "");
        return config;
    }
    try {
        config.secretAccessKey = keychainSecret(keychainService, config.accessKeyId);
    } catch (const std::runtime_error&) {
    }
    if (config.secretAccessKey.empty())
        throw std::runtime_error("no R2 secret access key: add it to the login keychain with `security "
                                 "add-generic-password -s " + keychainService + " -a " + config.accessKeyId +
                                 " -w`, or set " + env + "_SECRET_ACCESS_KEY");
    return config;
}

Conversion convertForPlayback(const fs::path& source, const fs::path& outDir, const ConvertOptions& options)
{
    const bool hap = options.codec != "h264";
    const fs::path target = outDir / (source.stem().string() + "." + sha256File(source).substr(0, 16) +
                                      (hap ? "." + options.codec + ".mov" : ".mp4"));
    if (fs::exists(target))
        return {target, "cached"};
    const Probe p = probe(source);
    if (hap) {
        fs::create_directories(outDir);
        const fs::path part = target.string() + ".part.mov";
        try {
            convertToHap(source, p, part, options);
        } catch (...) {
            fs::remove(part);
            throw;
        }
        fs::rename(part, target);
        return {target, "hap"};
    }
    std::string kind = "encoded";
    std::vector<std::string> args;
    if (isStill(p.codec)) {
        kind = "still";
        args = stillArgs(source, options);
    } else if (p.codec == "h264" && p.width <= 1920 && p.height <= 1080 && hasOneSecondGop(source, p.duration)) {
        kind = "remuxed";
        args = remuxArgs(source);
    } else {
        args = encodeArgs(source, p.fps, options);
    }
    fs::create_directories(outDir);
    const fs::path part = target.string() + ".part.mp4";
    try {
        runTool(ffmpegCommand(args, part));
    } catch (...) {
        fs::remove(part);
        throw;
    }
    fs::rename(part, target);
    return {target, kind};
}

UploadResult uploadObject(S3Client& client, const fs::path& file, const std::string& key, const UploadOptions& options)
{
    const uint64_t size = fs::file_size(file);
    const auto present = retrying(options.attempts, [&] { return client.head(key); });
    if (present && *present == size)
        return {"present"};
    if (size > options.partSize)
        return uploadParts(client, file, key, size, options);
    report(options, 0, size);
    const std::string body = readRange(file, 0, size);
    retrying(options.attempts, [&] { client.put(key, body, kContentType); });
    report(options, size, size);
    return {"uploaded", 1};
}

nlohmann::json runUpload(S3Client* client, const std::vector<fs::path>& sources, const RunOptions& options)
{
    nlohmann::json manifest = {{"started", utcText("%Y-%m-%dT%H:%M:%SZ")}};
    if (client)
        manifest.update({{"endpoint", client->config().endpoint}, {"bucket", client->config().bucket}});
    nlohmann::json files = nlohmann::json::array();
    std::map<std::string, int> summary;
    std::string refused;
    UploadStates states(options.states);
    for (const fs::path& source : sources) {
        nlohmann::json entry = {{"source", fs::absolute(source).string()}};
        UploadState state{.source = entry["source"], .pid = getpid()};
        try {
            const Conversion conversion = convertForPlayback(source, options.convertDir, options.convert);
            entry["converted"] = conversion.file.string();
            entry["conversion"] = conversion.kind;
            entry["hash"] = sha256File(conversion.file);
            entry["size"] = fs::file_size(conversion.file);
            entry["status"] = "converted";
            if (client) {
                state.size = entry["size"];
                states.set(entry["hash"], state);
                UploadOptions upload = options.upload;
                upload.progress = [&](uint64_t bytes, uint64_t size) {
                    state.bytes = bytes;
                    states.set(entry["hash"], state);
                    if (options.upload.progress)
                        options.upload.progress(bytes, size);
                };
                const UploadResult result = uploadObject(*client, conversion.file, entry["hash"], upload);
                state.kind = UploadState::Kind::Uploaded;
                state.bytes = state.size;
                states.set(entry["hash"], state);
                entry["status"] = result.state;
                entry["parts"] = result.parts;
                entry["resumed_parts"] = result.resumedParts;
            }
        } catch (const S3Error& e) {
            entry["status"] = "failed";
            entry["error"] = e.status == 401 || e.status == 403 ? refused = credentialsRefused(client->config(), e)
                                                                : e.what();
        } catch (const std::exception& e) {
            entry["status"] = "failed";
            entry["error"] = e.what();
        }
        if (entry["status"] == "failed" && client && entry.contains("hash")) {
            state.kind = UploadState::Kind::Failed;
            state.error = entry["error"];
            states.set(entry["hash"], state);
        }
        summary[entry["status"]]++;
        files.push_back(entry);
        if (!refused.empty())
            break;
    }
    manifest["files"] = files;
    manifest["summary"] = summary;
    manifest["finished"] = utcText("%Y-%m-%dT%H:%M:%SZ");
    fs::create_directories(options.manifest.parent_path());
    writeFileAtomically(options.manifest, manifest.dump(2) + "\n");
    if (!refused.empty())
        throw std::runtime_error(refused);
    return manifest;
}

std::vector<fs::path> mediaSources(const std::vector<fs::path>& arguments)
{
    static const std::set<std::string> extensions{".mp4", ".mov", ".m4v", ".mkv", ".avi", ".webm", ".png",
                                                  ".jpg", ".jpeg", ".tif", ".tiff", ".bmp", ".webp"};
    std::vector<fs::path> files;
    for (const fs::path& argument : arguments) {
        if (!fs::is_directory(argument)) {
            files.push_back(argument);
            continue;
        }
        std::vector<fs::path> found;
        for (const fs::directory_entry& e : fs::directory_iterator(argument)) {
            std::string extension = e.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
            if (e.is_regular_file() && e.path().filename().string()[0] != '.' && extensions.count(extension))
                found.push_back(e.path());
        }
        std::sort(found.begin(), found.end());
        files.insert(files.end(), found.begin(), found.end());
    }
    return files;
}

fs::path newManifestPath()
{
    return defaultUploadsRoot() / "manifests" / (utcText("%Y%m%dT%H%M%SZ") + ".json");
}

std::string manifestLine(const nlohmann::json& file)
{
    char status[16];
    std::snprintf(status, sizeof status, "%-9s ", file["status"].get<std::string>().c_str());
    return status + file["source"].get<std::string>() +
           (file.contains("hash") ? " -> " + file["hash"].get<std::string>() : "") +
           (file.contains("error") ? ": " + file["error"].get<std::string>() : "");
}

} // namespace azl
