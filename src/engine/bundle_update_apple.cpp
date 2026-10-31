#include "engine/bundle_update.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <mach-o/dyld.h>

#include <array>
#include <climits>
#include <memory>
#include <type_traits>

namespace azl {

namespace fs = std::filesystem;

namespace {

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

} // namespace azl
