#include "platform/credentials.hpp"

#include <windows.h>

#include <wincred.h>

namespace azl {

namespace {

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()));
    return out;
}

std::string utf8(const wchar_t* text, size_t size)
{
    if (size == 0)
        return {};
    std::string out(WideCharToMultiByte(CP_UTF8, 0, text, int(size), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, int(size), out.data(), int(out.size()), nullptr, nullptr);
    return out;
}

} // namespace

std::optional<std::string> readCredential(const std::string& service, const std::string& account)
{
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(wide(service).c_str(), CRED_TYPE_GENERIC, 0, &credential))
        return std::nullopt;
    std::optional<std::string> secret;
    if (credential->UserName && wide(account) == credential->UserName)
        secret = utf8(reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
                      credential->CredentialBlobSize / sizeof(wchar_t));
    CredFree(credential);
    return secret;
}

} // namespace azl
