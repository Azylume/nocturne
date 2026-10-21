#include "engine/isf_library.hpp"

#include "engine/isf_compiler.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace azl {

namespace {

namespace fs = std::filesystem;

std::vector<fs::path> shaderFiles(const fs::path& folder)
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".fs")
            files.push_back(it->path());
    std::sort(files.begin(), files.end());
    return files;
}

// What goes to clients must be valid UTF-8.
bool utf8(const std::string& text)
{
    try {
        return !nlohmann::json(text).dump().empty();
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

std::string readText(const fs::path& file, std::string& error)
{
    std::error_code ec;
    if (fs::file_size(file, ec) > UserIsfLibrary::kMaxFileBytes) {
        error = "the file is larger than " + std::to_string(UserIsfLibrary::kMaxFileBytes / 1024) + " KB";
        return {};
    }
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    if (!in)
        error = "the file cannot be read";
    else if (!utf8(text.str()))
        error = "the file is not UTF-8 text";
    return error.empty() ? text.str() : std::string();
}

bool same(const UserIsfShader& a, const UserIsfShader& b)
{
    return a.file == b.file && a.type == b.type && a.error == b.error && a.text == b.text;
}

} // namespace

UserIsfLibrary::UserIsfLibrary(fs::path folder, fs::path cacheDir)
    : folder_(std::move(folder)), cacheDir_(std::move(cacheDir))
{
    std::error_code ec;
    if (!folder_.empty())
        fs::create_directories(folder_, ec);
    scan();
}

UserIsfLibrary::~UserIsfLibrary()
{
    std::lock_guard lock(mutex_);
    for (const auto& [type, text] : effects_)
        retireEffect(type);
    IsfTransitions transitions = *isfTransitions();
    std::erase_if(transitions, [&](const IsfTransition& t) {
        return std::find(transitions_.begin(), transitions_.end(), t.mode) != transitions_.end();
    });
    setIsfTransitions(std::move(transitions));
}

bool UserIsfLibrary::scan()
{
    std::lock_guard lock(mutex_);
    std::vector<UserIsfShader> shaders;
    std::map<std::string, std::string> effects;
    std::vector<IsfTransition> transitions;
    std::set<std::string> taken;
    for (const fs::path& file : folder_.empty() ? std::vector<fs::path>{} : shaderFiles(folder_)) {
        UserIsfShader entry{.file = file.filename().string(), .name = file.stem().string()};
        entry.text = readText(file, entry.error);
        const auto parsed = entry.error.empty() ? parseIsf({entry.name, entry.text, true})
                                                : std::variant<IsfShader, std::string>(entry.error);
        if (const auto* error = std::get_if<std::string>(&parsed)) {
            entry.error = *error;
            shaders.push_back(std::move(entry));
            continue;
        }
        const IsfShader& shader = std::get<IsfShader>(parsed);
        entry.type = shader.type;
        entry.kind = shader.kind;
        if (!taken.insert(shader.type).second) {
            entry.error = "another file is already " + shader.type;
            shaders.push_back(std::move(entry));
            continue;
        }
        const IsfMetal metal = translateIsf(shader, cacheDir_);
        entry.error = metal.error.substr(0, metal.error.find_last_not_of('\n') + 1);
        if (!utf8(entry.error))
            entry.error = "the shader does not translate";
        if (shader.kind == IsfKind::Transition) {
            if (metal.error.empty()) {
                IsfTransition transition = isfTransition(shader);
                transition.globals = metal.globals;
                transition.function = metal.body;
                transitions.push_back(std::move(transition));
            }
        } else {
            if (effects_.find(shader.type) == effects_.end() || effects_.at(shader.type) != entry.text)
                registerEffect(isfEffectDef(shader, metal));
            effects[shader.type] = entry.text;
        }
        if (!entry.error.empty())
            std::fprintf(stderr, "effects: user ISF %s: %s\n", entry.file.c_str(), entry.error.c_str());
        shaders.push_back(std::move(entry));
    }
    for (const auto& [type, text] : effects_)
        if (!effects.contains(type))
            retireEffect(type);
    effects_ = std::move(effects);

    IsfTransitions all = *isfTransitions();
    std::erase_if(all, [&](const IsfTransition& t) {
        return std::find(transitions_.begin(), transitions_.end(), t.mode) != transitions_.end();
    });
    transitions_.clear();
    for (IsfTransition& t : transitions) {
        transitions_.push_back(t.mode);
        all.push_back(std::move(t));
    }
    setIsfTransitions(std::move(all));

    const bool changed = !std::equal(shaders.begin(), shaders.end(), shaders_.begin(), shaders_.end(), same);
    shaders_ = std::move(shaders);
    return changed;
}

std::vector<UserIsfShader> UserIsfLibrary::shaders() const
{
    std::lock_guard lock(mutex_);
    return shaders_;
}

} // namespace azl
