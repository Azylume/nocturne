#include "net/pairing.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace azl {

namespace fs = std::filesystem;

namespace {

constexpr size_t kTokenBytes = 32;

std::string randomToken()
{
    unsigned char bytes[kTokenBytes];
    arc4random_buf(bytes, sizeof bytes);
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

} // namespace

fs::path defaultStateFolder()
{
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "/tmp") / "Library/Application Support/Nocturne/State";
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
        ::chmod(file.c_str(), S_IRUSR | S_IWUSR);
        return token;
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path part = file.string() + "." + std::to_string(::getpid()) + ".part";
    fs::remove(part, ec);
    const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0)
        throw std::runtime_error("cannot write the pairing token to " + part.string() + ": " + std::strerror(errno));
    const std::string token = randomToken();
    const std::string line = token + "\n";
    const bool written = ::write(fd, line.data(), line.size()) == ssize_t(line.size());
    ::close(fd);
    // A link fails when another process made the file first; its token wins.
    if (!written || (::link(part.c_str(), file.c_str()) != 0 && errno != EEXIST)) {
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

} // namespace azl
