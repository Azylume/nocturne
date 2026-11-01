// httpRequest on WinHTTP: the same blocking request as http.mm, with no cookies or cache and a 120 s timeout.

#include "upload/http.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <memory>
#include <string>

namespace azl {

namespace {

constexpr int kTimeoutMs = 120000;

struct CloseInternet {
    void operator()(HINTERNET handle) const { WinHttpCloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseInternet>;

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size);
    return out;
}

std::string narrow(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string lowercase(std::string text)
{
    for (char& c : text)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trimmed(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t");
    const size_t last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}

// The last WinHTTP error in words.
std::string lastError()
{
    const DWORD error = GetLastError();
    char* text = nullptr;
    const DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
                                          FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      GetModuleHandleW(L"winhttp.dll"), error, 0, reinterpret_cast<char*>(&text), 0,
                                      nullptr);
    std::string message = size ? trimmed(std::string(text, size)) : "error " + std::to_string(error);
    LocalFree(text);
    return message;
}

HINTERNET session()
{
    static const Handle shared = [] {
        HINTERNET s = WinHttpOpen(L"Nocturne", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
        if (s)
            WinHttpSetTimeouts(s, 0, kTimeoutMs, kTimeoutMs, kTimeoutMs);
        return Handle(s);
    }();
    return shared.get();
}

// "Name: value" lines, by lowercase name; repeated ones joined by ", ".
std::map<std::string, std::string> parseHeaders(const std::string& raw)
{
    std::map<std::string, std::string> headers;
    for (size_t at = raw.find("\r\n"); at != std::string::npos && at + 2 < raw.size();) {
        const size_t next = raw.find("\r\n", at + 2);
        const std::string line = raw.substr(at + 2, next == std::string::npos ? std::string::npos : next - at - 2);
        at = next;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string& value = headers[lowercase(trimmed(line.substr(0, colon)))];
        value += (value.empty() ? "" : ", ") + trimmed(line.substr(colon + 1));
    }
    return headers;
}

std::string rawHeaders(HINTERNET request)
{
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &size,
                             WINHTTP_NO_HEADER_INDEX))
        return {};
    text.resize(size / sizeof(wchar_t));
    return narrow(text);
}

} // namespace

HttpResponse httpRequest(const HttpRequest& request)
{
    const std::string where = request.method + " " + request.url;
    auto fail = [&](const std::string& why) { return TransportError(where + ": " + why); };

    const std::wstring url = wide(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts))
        throw TransportError("bad URL " + request.url);
    if (!session())
        throw fail(lastError());
    auto part = [](const wchar_t* text, DWORD size) { return text ? std::wstring(text, size) : std::wstring(); };
    const std::wstring host = part(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path =
        part(parts.lpszUrlPath, parts.dwUrlPathLength) + part(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    const Handle connection(WinHttpConnect(session(), host.c_str(), parts.nPort, 0));
    if (!connection)
        throw fail(lastError());
    const DWORD secure = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const wchar_t* target = path.empty() ? L"/" : path.c_str();
    const Handle r(WinHttpOpenRequest(connection.get(), wide(request.method).c_str(), target, nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure));
    if (!r)
        throw fail(lastError());
    DWORD noCookies = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(r.get(), WINHTTP_OPTION_DISABLE_FEATURE, &noCookies, sizeof noCookies);

    // WinHTTP writes Host from the URL and Content-Length from the body itself.
    std::wstring headers;
    for (const auto& [name, value] : request.headers)
        if (lowercase(name) != "host" && lowercase(name) != "content-length")
            headers += wide(name + ": " + value + "\r\n");
    const DWORD add = WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE;
    if (!headers.empty() && !WinHttpAddRequestHeaders(r.get(), headers.c_str(), DWORD(-1), add))
        throw fail(lastError());

    void* body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data());
    const DWORD length = DWORD(request.body.size());
    if (!WinHttpSendRequest(r.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, length, length, 0) ||
        !WinHttpReceiveResponse(r.get(), nullptr))
        throw fail(lastError());

    HttpResponse response;
    DWORD status = 0;
    DWORD size = sizeof status;
    WinHttpQueryHeaders(r.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    response.status = int(status);
    response.headers = parseHeaders(rawHeaders(r.get()));
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(r.get(), &available))
            throw fail(lastError());
        if (available == 0)
            break;
        const size_t at = response.body.size();
        response.body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(r.get(), response.body.data() + at, available, &read))
            throw fail(lastError());
        response.body.resize(at + read);
    }
    return response;
}

} // namespace azl
