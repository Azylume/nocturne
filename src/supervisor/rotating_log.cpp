#include "supervisor/rotating_log.hpp"

#include <stdexcept>
#include <system_error>

namespace azl {

namespace {

std::filesystem::path numbered(const std::filesystem::path& path, int n)
{
    return path.string() + "." + std::to_string(n);
}

} // namespace

RotatingLog::RotatingLog(std::filesystem::path path, uint64_t maxBytes, int keep)
    : path_(std::move(path)), maxBytes_(maxBytes), keep_(keep)
{
    std::error_code ec;
    if (path_.has_parent_path())
        std::filesystem::create_directories(path_.parent_path(), ec);
    open();
    if (!file_)
        throw std::runtime_error("cannot open " + path_.string());
}

RotatingLog::~RotatingLog()
{
    if (file_)
        std::fclose(file_);
}

void RotatingLog::write(const std::string& text)
{
    std::lock_guard lock(mutex_);
    if (!file_)
        return;
    if (size_ > 0 && size_ + text.size() > maxBytes_)
        rotate();
    if (!file_)
        return;
    std::fwrite(text.data(), 1, text.size(), file_);
    std::fflush(file_);
    size_ += text.size();
}

void RotatingLog::open()
{
    file_ = std::fopen(path_.c_str(), "a");
    std::error_code ec;
    const auto size = std::filesystem::file_size(path_, ec);
    size_ = ec ? 0 : uint64_t(size);
}

void RotatingLog::rotate()
{
    std::fclose(file_);
    file_ = nullptr;
    std::error_code ec;
    std::filesystem::remove(numbered(path_, keep_), ec);
    for (int n = keep_ - 1; n >= 1; --n)
        std::filesystem::rename(numbered(path_, n), numbered(path_, n + 1), ec);
    if (keep_ > 0)
        std::filesystem::rename(path_, numbered(path_, 1), ec);
    else
        std::filesystem::remove(path_, ec);
    open();
}

} // namespace azl
