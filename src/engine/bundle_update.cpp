#include "engine/bundle_update.hpp"

#include "document/composition_file.hpp"
#include "platform/process.hpp"
#include "protocol/bundle_push.hpp"

#include <cstdio>
#include <map>
#include <vector>

namespace azl {

namespace fs = std::filesystem;

namespace {

Message error(const std::string& text)
{
    return {{"type", "error"}, {"op", "update"}, {"message", text}};
}

} // namespace

InstallBundle bundleInstaller(fs::path script, std::map<std::string, std::string> environment)
{
    return [script = std::move(script), environment = std::move(environment)](const fs::path& app)
               -> std::optional<std::string> {
        if (!fs::exists(script))
            return "no installer at " + script.string();
        std::string error;
        const std::optional<Process> process =
            spawnProcess({.program = "/bin/bash",
                          .args = {script.string(), app.string()},
                          .input = Stdio::null(),
                          .output = Stdio::null(),
                          .error = Stdio::null(),
                          .environment = {environment.begin(), environment.end()},
                          .detached = true},
                         &error);
        if (!process)
            return "cannot start the installer: " + error;
        std::printf("update: installer started (pid %d) on %s\n", process->pid(), app.string().c_str());
        std::fflush(stdout);
        return std::nullopt;
    };
}

BundleUpdate::BundleUpdate(UpdateSource source) : source_(std::move(source)) {}

Message BundleUpdate::receive(const Message& message, const std::function<std::optional<std::string>()>& refusal)
{
    std::lock_guard lock(mutex_);
    if (!served())
        return error("Update is not served by this engine");
    if (message.contains("size")) {
        if (const std::optional<std::string> refused = refusal()) {
            drop();
            return error(*refused);
        }
        return start(message);
    }
    if (message.contains("data"))
        return part(message, refusal);
    return error("update needs \"size\" to start or \"offset\" and \"data\" for a part");
}

Message BundleUpdate::start(const Message& message)
{
    drop();
    if (!message["size"].is_number_unsigned() || message["size"].get<uint64_t>() == 0 ||
        message["size"].get<uint64_t>() > kMaxBundleSize)
        return error("update needs a size of 1 byte to 1 GB");
    size_ = message["size"].get<uint64_t>();
    build_ = message.value("build", "");
    std::error_code ec;
    fs::remove_all(source_.incoming, ec);
    fs::create_directories(source_.incoming, ec);
    out_.open(source_.incoming / "bundle.tgz", std::ios::binary | std::ios::trunc);
    if (!out_) {
        drop();
        return error("cannot write to " + source_.incoming.string());
    }
    std::printf("update: receiving build %s (%llu bytes)\n", build_.c_str(), (unsigned long long)size_);
    std::fflush(stdout);
    return {{"type", "machine"}, {"op", "update"}, {"state", "receiving"}, {"received", 0}, {"size", size_}};
}

Message BundleUpdate::part(const Message& message, const std::function<std::optional<std::string>()>& refusal)
{
    if (!out_.is_open())
        return error("no update in progress; start one with its size");
    if (!message["data"].is_binary() || message.value("offset", uint64_t(-1)) != received_) {
        drop();
        return error("update part out of order: expected offset " + std::to_string(received_));
    }
    const auto& data = message["data"].get_binary();
    if (data.size() > kBundleChunk || received_ + data.size() > size_) {
        drop();
        return error("update part too large");
    }
    out_.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out_) {
        drop();
        return error("cannot write the update to " + source_.incoming.string() + "; is the disk full?");
    }
    received_ += data.size();
    if (received_ < size_)
        return {{"type", "machine"}, {"op", "update"}, {"state", "receiving"}, {"received", received_}, {"size", size_}};
    return install(refusal);
}

Message BundleUpdate::install(const std::function<std::optional<std::string>()>& refusal)
{
    out_.close();
    if (const std::optional<std::string> refused = refusal()) {
        drop();
        return error(*refused);
    }
    const fs::path unpacked = source_.incoming / "unpacked";
    const fs::path app = unpacked / "Nocturne.app";
    std::error_code ec;
    fs::create_directories(unpacked, ec);
    if (runProgram({tarProgram().string(), "-xzf", (source_.incoming / "bundle.tgz").string(), "-C", unpacked.string()}) != 0) {
        drop();
        return error("the update arrived damaged: it does not unpack");
    }
    fs::remove(source_.incoming / "bundle.tgz", ec);
    const std::string build = bundleBuildId(app);
    if (build.empty() || (!build_.empty() && build != build_)) {
        drop();
        return error("the update holds no Nocturne.app of build " + build_);
    }
    if (const std::optional<std::string> failed = source_.install(app)) {
        drop();
        return error(*failed);
    }
    size_ = received_ = 0;
    return {{"type", "machine"}, {"op", "update"}, {"state", "installing"}, {"build", build}};
}

void BundleUpdate::drop()
{
    if (out_.is_open())
        out_.close();
    out_.clear();
    size_ = received_ = 0;
}

Message BundleUpdate::lastResult() const
{
    if (source_.result.empty())
        return nullptr;
    try {
        const std::optional<nlohmann::json> result = readJsonFile(source_.result);
        return result && result->is_object() ? *result : Message(nullptr);
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace azl
