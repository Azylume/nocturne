// The Windows twin of venues_test.cpp's API key case: the Credential Manager stands in for the keychain, and a key in
// the venues file is refused because Windows gives the file no owner-only permission to check.
#include "scoped_env.hpp"
#include "temp_folder.hpp"

#include "platform/credentials.hpp"
#include "venues/venues.hpp"

#include <doctest.h>

#include <windows.h>

#include <wincred.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

void writeFile(const fs::path& path, const std::string& text)
{
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

// Stores the Tailscale key as `cmdkey /generic:nocturne-tailscale /user:api-key /pass` would, and removes it again.
class StoredKey {
public:
    explicit StoredKey(const std::wstring& secret)
    {
        CREDENTIALW credential{};
        credential.Type = CRED_TYPE_GENERIC;
        credential.TargetName = const_cast<wchar_t*>(kTarget);
        credential.UserName = const_cast<wchar_t*>(L"api-key");
        credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(secret.c_str()));
        credential.CredentialBlobSize = DWORD(secret.size() * sizeof(wchar_t));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        REQUIRE(CredWriteW(&credential, 0));
    }
    ~StoredKey() { CredDeleteW(kTarget, CRED_TYPE_GENERIC, 0); }

private:
    static constexpr const wchar_t* kTarget = L"nocturne-tailscale";
};

} // namespace

TEST_CASE("the API key comes from the Credential Manager or the environment, never from a file on Windows")
{
    TempFolder dir;
    const fs::path file = dir.path / "venues.json";
    const auto none = [] { return std::string(); };

    azl::TailscaleSettings settings = azl::loadTailscaleSettings(file, none);
    CHECK(settings.apiKey.empty());
    CHECK(contains(settings.keyProblem, "no Tailscale API key"));
    CHECK(contains(settings.keyProblem, "cmdkey /generic:nocturne-tailscale /user:api-key /pass"));

    writeFile(file, R"({"tailnet": "example.com", "tag": "tag:club", "port": 7410})");
    settings = azl::loadTailscaleSettings(file, [] { return std::string("tskey-from-credentials"); });
    CHECK(settings.tailnet == "example.com");
    CHECK(settings.tag == "tag:club");
    CHECK(settings.port == 7410);
    CHECK(settings.apiKey == "tskey-from-credentials");
    CHECK(settings.keyProblem.empty());

    writeFile(file, R"({"api_key": "tskey-in-file"})");
    settings = azl::loadTailscaleSettings(file, [] { return std::string("tskey-from-credentials"); });
    CHECK(settings.apiKey.empty());
    CHECK(contains(settings.keyProblem, "keep the key in the Credential Manager"));

    if (azl::readCredential(azl::kTailscaleKeychainService, azl::kTailscaleKeychainAccount)) {
        MESSAGE("skipped the Credential Manager part: this machine holds a real Tailscale key");
        return;
    }
    ScopedEnv unset("NOCTURNE_TAILSCALE_API_KEY", nullptr);
    CHECK(azl::systemTailscaleKey().empty());
    {
        StoredKey stored(L"tskey-from-credentials");
        CHECK(azl::systemTailscaleKey() == "tskey-from-credentials");
        ScopedEnv set("NOCTURNE_TAILSCALE_API_KEY", "tskey-from-env");
        CHECK(azl::systemTailscaleKey() == "tskey-from-env");
    }
    CHECK(azl::systemTailscaleKey().empty());
}
