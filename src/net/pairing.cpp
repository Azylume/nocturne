#include "net/pairing.hpp"

#include "net/lan.hpp"
#include "platform/app_paths.hpp"
#include "platform/crypto.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <share.h>
#include <windows.h>
#include <sddl.h>
#else
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace azl {

namespace fs = std::filesystem;

namespace {

constexpr size_t kTokenBytes = 32;

std::string randomToken()
{
    unsigned char bytes[kTokenBytes];
    randomBytes(bytes, sizeof bytes);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    for (unsigned char b : bytes) {
        text += kHex[b >> 4];
        text += kHex[b & 15];
    }
    return text;
}

bool tokenText(const std::string& text)
{
    return text.size() == 2 * kTokenBytes && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

#ifdef _WIN32

// Only this user may open the file, whatever the folder lets others do: one protected access entry, like mode 0600.
void ownerOnly(const fs::path& file)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<uint8_t> user(size);
    LPWSTR sid = nullptr;
    if (GetTokenInformation(token, TokenUser, user.data(), size, &size) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) {
        const std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
            SetFileSecurityW(file.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
            LocalFree(descriptor);
        }
        LocalFree(sid);
    }
    CloseHandle(token);
}

// A new file at `path` for its owner alone to write; negative when one exists or it cannot be made.
int openNew(const fs::path& path)
{
    int fd = -1;
    _wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE);
    if (fd >= 0)
        ownerOnly(path);
    return fd;
}

bool writeAndClose(int fd, const std::string& line)
{
    const bool written = _write(fd, line.data(), unsigned(line.size())) == int(line.size());
    _close(fd);
    return written;
}

int processId()
{
    return _getpid();
}

#else

void ownerOnly(const fs::path& file)
{
    ::chmod(file.c_str(), S_IRUSR | S_IWUSR);
}

int openNew(const fs::path& path)
{
    return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
}

bool writeAndClose(int fd, const std::string& line)
{
    const bool written = ::write(fd, line.data(), line.size()) == ssize_t(line.size());
    ::close(fd);
    return written;
}

int processId()
{
    return int(::getpid());
}

#endif

} // namespace

fs::path defaultStateFolder()
{
    return appDataFolder() / "State";
}

fs::path tokenFile(const fs::path& state)
{
    return state / "token";
}

std::string readToken(const fs::path& file)
{
    std::ifstream in(file);
    std::string token;
    in >> token;
    return tokenText(token) ? token : std::string();
}

std::string machineToken(const fs::path& file)
{
    if (std::string token = readToken(file); !token.empty()) {
        ownerOnly(file);
        return token;
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path part = file.string() + "." + std::to_string(processId()) + ".part";
    fs::remove(part, ec);
    const int fd = openNew(part);
    if (fd < 0)
        throw std::runtime_error("cannot write the pairing token to " + part.string() + ": " + std::strerror(errno));
    const bool written = writeAndClose(fd, randomToken() + "\n");
    // A link fails when another process made the file first; its token wins.
    if (written)
        fs::create_hard_link(part, file, ec);
    if (!written || (ec && ec != std::errc::file_exists)) {
        fs::remove(part, ec);
        throw std::runtime_error("cannot write the pairing token to " + file.string());
    }
    fs::remove(part, ec);
    if (std::string kept = readToken(file); !kept.empty())
        return kept;
    throw std::runtime_error(file.string() + " holds no pairing token");
}

bool sameToken(const std::string& presented, const std::string& token)
{
    if (token.empty() || presented.size() != token.size())
        return false;
    unsigned char differ = 0;
    for (size_t i = 0; i < token.size(); ++i)
        differ |= static_cast<unsigned char>(presented[i] ^ token[i]);
    return differ == 0;
}

bool presentsToken(bool named, const std::string& host)
{
    return named || host == "localhost" || loopbackAddress(host);
}

} // namespace azl
