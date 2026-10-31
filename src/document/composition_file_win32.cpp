#include "document/composition_file.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include <windows.h>

namespace azl {

namespace {

// A reader that opened the file without sharing its deletion, as std::ifstream does, makes the replace fail while it
// holds the file open. Readers hold it for a moment, so this tries again for up to a second.
bool replaceFile(const std::filesystem::path& from, const std::filesystem::path& to)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        const DWORD error = GetLastError();
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) ||
            std::chrono::steady_clock::now() >= until)
            return false;
        Sleep(1);
    }
}

} // namespace

void writeFileAtomically(const std::filesystem::path& path, const std::string& contents, bool sync)
{
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot write " + temporary.string());
    // A full disk leaves no partial temporary file behind to take what space frees up.
    auto fail = [&](const std::string& what) {
        CloseHandle(file);
        DeleteFileW(temporary.c_str());
        throw std::runtime_error(what);
    };
    size_t written = 0;
    while (written < contents.size()) {
        DWORD n = 0;
        const DWORD chunk = DWORD(std::min<size_t>(contents.size() - written, 1u << 30));
        if (!WriteFile(file, contents.data() + written, chunk, &n, nullptr) || n == 0)
            fail("cannot write " + temporary.string());
        written += n;
    }
    if (sync && !FlushFileBuffers(file))
        fail("cannot flush " + temporary.string());
    if (!CloseHandle(file)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("cannot write " + temporary.string());
    }
    if (!replaceFile(temporary, path)) {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("cannot rename " + temporary.string() + " to " + path.string());
    }
}

} // namespace azl
