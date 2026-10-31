#include <doctest.h>

#include "platform/credentials.hpp"

#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>

#include <wincred.h>
#endif

TEST_CASE("platform: a credential nobody stored reads as nothing")
{
    CHECK_FALSE(azl::readCredential("nocturne-test-never-stored", "nobody"));
}

#ifdef _WIN32
TEST_CASE("platform: a credential stored in the Credential Manager reads back for its account only")
{
    const std::wstring target = L"nocturne-test-credential", user = L"tester", secret = L"s3cret";
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(target.c_str());
    credential.UserName = const_cast<wchar_t*>(user.c_str());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(secret.c_str()));
    credential.CredentialBlobSize = DWORD(secret.size() * sizeof(wchar_t));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    REQUIRE(CredWriteW(&credential, 0));
    CHECK(azl::readCredential("nocturne-test-credential", "tester") == std::optional<std::string>("s3cret"));
    CHECK_FALSE(azl::readCredential("nocturne-test-credential", "someone-else"));
    CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
}
#endif
