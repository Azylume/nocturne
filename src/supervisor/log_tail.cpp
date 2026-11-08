#include "supervisor/log_tail.hpp"

#include <algorithm>
#include <fstream>

namespace azl {

LogTail tailLog(const std::filesystem::path& file, size_t lines)
{
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in)
        return {};
    const std::streamoff size = in.tellg();
    const std::streamoff start = std::max<std::streamoff>(0, size - std::streamoff(kMaxLogBytes));
    std::string text(size_t(size - start), '\0');
    in.seekg(start);
    in.read(text.data(), std::streamsize(text.size()));
    text.resize(size_t(std::max<std::streamsize>(0, in.gcount())));

    size_t from = 0;
    if (start > 0) {
        const size_t newline = text.find('\n');
        from = newline == std::string::npos ? text.size() : newline + 1;
    }
    const size_t wanted = std::clamp<size_t>(lines, 1, kMaxLogLines);
    size_t seen = 0;
    for (size_t i = text.size(); i-- > from;)
        if (text[i] == '\n' && i + 1 != text.size() && ++seen == wanted) {
            from = i + 1;
            break;
        }
    return {text.substr(from), start > 0 || from > 0};
}

} // namespace azl
