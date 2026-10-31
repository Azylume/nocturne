#include "document/composition_file.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace azl {

void writeFileAtomically(const std::filesystem::path& path, const std::string& contents, bool sync)
{
    const std::filesystem::path temporary = path.string() + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::runtime_error("cannot write " + temporary.string() + ": " + std::strerror(errno));
    // A full disk leaves no partial temporary file behind to take what space frees up.
    auto fail = [&](const std::string& what) {
        const std::string reason = what + ": " + std::strerror(errno);
        ::close(fd);
        ::unlink(temporary.c_str());
        throw std::runtime_error(reason);
    };
    size_t written = 0;
    while (written < contents.size()) {
        const ssize_t n = ::write(fd, contents.data() + written, contents.size() - written);
        if (n <= 0)
            fail("cannot write " + temporary.string());
        written += size_t(n);
    }
    if (sync && ::fsync(fd) != 0)
        fail("cannot flush " + temporary.string());
    if (::close(fd) != 0) {
        ::unlink(temporary.c_str());
        throw std::runtime_error("cannot write " + temporary.string());
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        throw std::runtime_error("cannot rename " + temporary.string() + " to " + path.string());
    }
}

} // namespace azl
