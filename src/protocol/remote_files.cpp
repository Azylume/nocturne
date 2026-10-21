#include "protocol/remote_files.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>

namespace azl {

namespace fs = std::filesystem;

namespace {

fs::path expanded(const std::string& path)
{
    if (path.starts_with("~/"))
        if (const char* home = std::getenv("HOME"))
            return fs::path(home) / path.substr(2);
    return path;
}

fs::path resolved(const fs::path& path)
{
    std::error_code ec;
    fs::path real = fs::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : real;
}

bool within(const fs::path& path, const fs::path& root)
{
    const auto rootEnd = std::mismatch(root.begin(), root.end(), path.begin(), path.end()).first;
    return rootEnd == root.end() || (std::next(rootEnd) == root.end() && rootEnd->empty());
}

// The path fields of each message type and op.
const std::map<std::pair<std::string, std::string>, std::vector<std::string>>& pathFields()
{
    static const std::map<std::pair<std::string, std::string>, std::vector<std::string>> fields{
        {{"composition", "save"}, {"path", "mediaRoot"}},
        {{"composition", "load"}, {"path", "mediaRoot"}},
        {{"composition", "shows"}, {"folder"}},
        {{"library", "add"}, {"path"}},
        {{"library", "relink"}, {"path"}},
        {{"record", "set"}, {"path"}},
        {{"record", "folders"}, {"path"}},
        {{"preset", "import"}, {"roots"}},
    };
    return fields;
}

} // namespace

std::optional<std::string> pathRefusal(const std::string& text, const std::vector<fs::path>& roots)
{
    const fs::path path = expanded(text);
    if (!path.is_absolute())
        return text + " is not an absolute path";
    if (std::any_of(path.begin(), path.end(), [](const fs::path& part) { return part == ".."; }))
        return text + " has a \"..\" part";
    const fs::path real = resolved(path);
    for (const fs::path& root : roots)
        if (!root.empty() && within(real, resolved(root)))
            return std::nullopt;
    return text + " is outside the folders a client on another machine may use (the shows, media, library, recording "
                  "and state folders)";
}

std::optional<std::string> remotePathRefusal(const Message& message, const std::vector<fs::path>& roots)
{
    if (!message.is_object())
        return std::nullopt;
    const auto fields = pathFields().find({message.value("type", ""), message.value("op", "")});
    if (fields == pathFields().end())
        return std::nullopt;
    for (const std::string& name : fields->second) {
        const auto field = message.find(name);
        if (field == message.end())
            continue;
        std::vector<Message> paths = field->is_array() ? std::vector<Message>(field->begin(), field->end())
                                                       : std::vector<Message>{*field};
        for (const Message& path : paths) {
            if (!path.is_string())
                return name + " must be a path";
            if (path.get<std::string>().empty())
                continue;
            if (std::optional<std::string> refused = pathRefusal(path.get<std::string>(), roots))
                return refused;
        }
    }
    return std::nullopt;
}

} // namespace azl
