#include "engine/bundle_update.hpp"

#include "document/composition_file.hpp"
#include "protocol/bundle_push.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>

#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

extern char** environ;

namespace azl {

namespace fs = std::filesystem;

namespace {

Message error(const std::string& text)
{
    return {{"type", "error"}, {"op", "update"}, {"message", text}};
}

struct Released {
    void operator()(CFTypeRef ref) const { CFRelease(ref); }
};
template <typename Ref>
using Owned = std::unique_ptr<std::remove_pointer_t<Ref>, Released>;

constexpr SecCSFlags kStrict = kSecCSCheckAllArchitectures | kSecCSCheckNestedCode | kSecCSStrictValidate;

std::string statusText(OSStatus status)
{
    const Owned<CFStringRef> text(SecCopyErrorMessageString(status, nullptr));
    char buffer[256] = "";
    if (!text || !CFStringGetCString(text.get(), buffer, sizeof buffer, kCFStringEncodingUTF8))
        return "error " + std::to_string(status);
    return buffer;
}

Owned<SecStaticCodeRef> staticCode(const fs::path& path)
{
    const std::string text = path.string();
    const Owned<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(text.data()), CFIndex(text.size()), fs::is_directory(path)));
    SecStaticCodeRef code = nullptr;
    if (!url || SecStaticCodeCreateWithPath(url.get(), kSecCSDefaultFlags, &code) != errSecSuccess)
        return nullptr;
    return Owned<SecStaticCodeRef>(code);
}

} // namespace

std::optional<std::string> signerMissing(const fs::path& running)
{
    const Owned<SecStaticCodeRef> code = staticCode(running);
    CFDictionaryRef info = nullptr;
    if (!code || SecCodeCopySigningInformation(code.get(), kSecCSSigningInformation, &info) != errSecSuccess)
        return running.string() + " cannot be read as code";
    const Owned<CFDictionaryRef> owned(info);
    if (!CFDictionaryGetValue(info, kSecCodeInfoIdentifier))
        return running.string() + " is not signed, so no pushed bundle can be checked against its signer";
    const auto certificates = static_cast<CFArrayRef>(CFDictionaryGetValue(info, kSecCodeInfoCertificates));
    if (!certificates || CFArrayGetCount(certificates) == 0)
        return running.string() + " is signed ad hoc, so no pushed bundle can be checked against its signer; "
                                  "sign it with an identity (tools/sign-bundle.sh) and install it by hand";
    return std::nullopt;
}

std::optional<std::string> signerRefusal(const fs::path& running, const fs::path& candidate)
{
    if (std::optional<std::string> missing = signerMissing(running))
        return missing;
    const Owned<SecStaticCodeRef> own = staticCode(running);
    SecRequirementRef designated = nullptr;
    if (SecCodeCopyDesignatedRequirement(own.get(), kSecCSDefaultFlags, &designated) != errSecSuccess)
        return running.string() + " has no designated requirement";
    const Owned<SecRequirementRef> requirement(designated);
    const Owned<SecStaticCodeRef> code = staticCode(candidate);
    if (!code)
        return "the update is not signed code";
    const OSStatus status = SecStaticCodeCheckValidity(code.get(), kStrict, requirement.get());
    if (status == errSecCSReqFailed)
        return "the update is not signed by the identity that signed the running bundle";
    if (status != errSecSuccess)
        return "the update does not pass the signature check (" + statusText(status) + ")";
    return std::nullopt;
}

UpdateSource installedUpdate(const fs::path& state)
{
    std::array<char, PATH_MAX> path{};
    uint32_t size = path.size();
    if (_NSGetExecutablePath(path.data(), &size) != 0)
        return {};
    const fs::path bundle = fs::weakly_canonical(path.data()).parent_path().parent_path().parent_path();
    if (bundle.extension() != ".app")
        return {};
    const InstallBundle installer =
        bundleInstaller(bundle / "Contents/Resources/update-bundle.sh",
                        {{"VENUE", bundle.parent_path().string()}, {"STATE", state.string()}, {"DELAY", "1"}});
    return {bundle.parent_path() / "incoming",
            [bundle, installer](const fs::path& app) -> std::optional<std::string> {
                if (std::optional<std::string> refused = signerRefusal(bundle, app))
                    return refused;
                return installer(app);
            },
            state / "update.json", signerMissing(bundle)};
}

InstallBundle bundleInstaller(fs::path script, std::map<std::string, std::string> environment)
{
    return [script = std::move(script), environment = std::move(environment)](const fs::path& app)
               -> std::optional<std::string> {
        if (!fs::exists(script))
            return "no installer at " + script.string();
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        for (int fd : {0, 1, 2})
            posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT);
        std::vector<std::string> env;
        for (char** e = environ; *e; ++e)
            if (!environment.contains(std::string(std::string_view(*e).substr(0, std::string_view(*e).find('=')))))
                env.push_back(*e);
        for (const auto& [name, value] : environment)
            env.push_back(name + "=" + value);
        std::vector<char*> envp;
        for (std::string& e : env)
            envp.push_back(e.data());
        envp.push_back(nullptr);
        const std::string bash = "/bin/bash", path = script.string(), bundle = app.string();
        char* argv[] = {const_cast<char*>(bash.c_str()), const_cast<char*>(path.c_str()),
                        const_cast<char*>(bundle.c_str()), nullptr};
        pid_t pid = 0;
        const int failed = posix_spawn(&pid, bash.c_str(), &actions, &attributes, argv, envp.data());
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        if (failed)
            return "cannot start the installer: " + std::string(std::strerror(failed));
        std::printf("update: installer started (pid %d) on %s\n", int(pid), bundle.c_str());
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
    if (runProgram({"/usr/bin/tar", "-xzf", (source_.incoming / "bundle.tgz").string(), "-C", unpacked.string()}) != 0) {
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
