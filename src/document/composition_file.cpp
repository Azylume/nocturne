#include "document/composition_file.hpp"

#include <chrono>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace azl {

namespace {

constexpr const char* kFormat = "nocturne.composition";
constexpr const char* kPreviousFormat = "azylumen.composition";

bool knownFormat(const nlohmann::json& file)
{
    const std::string format = file.value("format", "");
    return format == kFormat || format == kPreviousFormat;
}

nlohmann::json playJson(const std::optional<Play>& play)
{
    if (!play)
        return nullptr;
    return {{"column", play->column}, {"beat", play->beat ? nlohmann::json(*play->beat) : nullptr}};
}

std::optional<Play> playFrom(const nlohmann::json& j)
{
    if (!j.is_object())
        return std::nullopt;
    Play play{j.at("column").get<uint64_t>(), std::nullopt};
    if (auto beat = j.find("beat"); beat != j.end() && !beat->is_null())
        play.beat = beat->get<double>();
    return play;
}

nlohmann::json choiceJson(const AudioChoice& choice)
{
    return {{"device", choice.device}, {"channels", choice.channels}};
}

AudioChoice choiceFrom(const nlohmann::json& j)
{
    return {j.value("device", std::string()), j.value("channels", std::vector<int>{})};
}

nlohmann::json resumeJson(const Resume& resume)
{
    nlohmann::json j{{"savedAt", resume.savedAt},
                     {"beat", resume.beat},
                     {"tempo", resume.tempo},
                     {"pilots", pilotsJson(resume.pilots)}};
    if (resume.audio) {
        j["audio"] = choiceJson(resume.audio->open);
        j["audio"]["commandLine"] = choiceJson(resume.audio->commandLine);
    }
    return j;
}

Resume resumeFrom(const nlohmann::json& j)
{
    Resume resume;
    resume.savedAt = j.at("savedAt").get<double>();
    resume.beat = j.at("beat").get<double>();
    resume.tempo = j.value("tempo", resume.tempo);
    resume.pilots = pilotsFrom(j.value("pilots", nlohmann::json::array()));
    if (auto audio = j.find("audio"); audio != j.end() && audio->is_object())
        resume.audio =
            ResumeAudio{choiceFrom(*audio), choiceFrom(audio->value("commandLine", nlohmann::json::object()))};
    return resume;
}

} // namespace

nlohmann::json pilotsJson(const std::vector<ResumePilot>& pilots)
{
    nlohmann::json j = nlohmann::json::array();
    for (const ResumePilot& p : pilots) {
        j.push_back({{"layer", p.layer},
                     {"trigger", playJson(p.trigger)},
                     {"play", playJson(p.play)},
                     {"stepStart", p.stepStart},
                     {"next", p.next}});
        if (p.held)
            j.back()["held"] = true;
    }
    return j;
}

std::vector<ResumePilot> pilotsFrom(const nlohmann::json& j)
{
    std::vector<ResumePilot> pilots;
    if (!j.is_array())
        return pilots;
    for (const nlohmann::json& p : j) {
        const std::optional<Play> play = playFrom(p.at("play"));
        if (!play)
            continue;
        pilots.push_back({p.at("layer").get<uint64_t>(), playFrom(p.value("trigger", nlohmann::json())), *play,
                          p.value("stepStart", 0.0), p.value("next", uint64_t(0)), p.value("held", false)});
    }
    return pilots;
}

nlohmann::json toJson(const CompositionFile& file)
{
    nlohmann::json j{{"format", kFormat}, {"version", kCompositionFileVersion}, {"composition", file.composition}};
    if (file.resume)
        j["resume"] = resumeJson(*file.resume);
    if (file.source)
        j["source"] = *file.source;
    if (file.show)
        j["show"] = *file.show;
    if (file.file)
        j["file"] = {{"path", file.file->path}, {"unsaved", file.file->unsaved}};
    return j;
}

CompositionFile compositionFileFromJson(const nlohmann::json& j)
{
    if (!j.is_object() || !knownFormat(j))
        throw ProtocolError("not a Nocturne composition file");
    const int version = j.value("version", 0);
    if (version < 1 || version > kCompositionFileVersion)
        throw ProtocolError("unsupported composition file version " + std::to_string(version));
    try {
        CompositionFile file;
        file.composition = j.at("composition").get<Composition>();
        if (auto resume = j.find("resume"); resume != j.end() && resume->is_object())
            file.resume = resumeFrom(*resume);
        if (auto source = j.find("source"); source != j.end() && source->is_string())
            file.source = source->get<std::string>();
        if (auto show = j.find("show"); show != j.end() && show->is_string())
            file.show = show->get<std::string>();
        if (auto showFile = j.find("file"); showFile != j.end() && showFile->is_object())
            file.file = ShowFile{showFile->value("path", ""), showFile->value("unsaved", false)};
        return file;
    } catch (const nlohmann::json::exception& e) {
        throw ProtocolError(std::string("bad composition file: ") + e.what());
    }
}

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

void saveCompositionFile(const std::filesystem::path& path, const CompositionFile& file)
{
    writeFileAtomically(path, toJson(file).dump(2) + "\n");
}

std::optional<nlohmann::json> readJsonFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in) {
        if (!std::filesystem::exists(path))
            return std::nullopt;
        throw std::runtime_error("cannot read " + path.string());
    }
    std::stringstream text;
    text << in.rdbuf();
    nlohmann::json j = nlohmann::json::parse(text.str(), nullptr, false);
    if (j.is_discarded())
        throw ProtocolError(path.string() + " is not valid JSON");
    return j;
}

std::optional<CompositionFile> loadCompositionFile(const std::filesystem::path& path)
{
    const std::optional<nlohmann::json> j = readJsonFile(path);
    if (!j)
        return std::nullopt;
    return compositionFileFromJson(*j);
}

std::optional<CompositionFile> loadCompositionFile(const std::filesystem::path& path, std::vector<std::string>& warnings)
{
    const std::optional<nlohmann::json> j = readJsonFile(path);
    if (!j)
        return std::nullopt;
    return compositionFileFromJson(*j, warnings);
}

namespace {

template <typename T>
bool parses(const nlohmann::json& j)
{
    try {
        (void)j.get<T>();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Drops the fields of `j` that keep it from parsing as T, one at a time, with a warning each. False when it still
// does not parse.
template <typename T>
bool repair(nlohmann::json& j, const std::string& what, std::vector<std::string>& warnings)
{
    if (parses<T>(j))
        return true;
    if (!j.is_object())
        return false;
    std::vector<std::string> keys;
    for (const auto& [key, value] : j.items())
        keys.push_back(key);
    for (const std::string& key : keys) {
        nlohmann::json without = j;
        without.erase(key);
        if (!parses<T>(without))
            continue;
        warnings.push_back(what + ": " + key + " " + j[key].dump() + " is not understood; default used");
        j = std::move(without);
        return true;
    }
    return false;
}

void repairLayers(nlohmann::json& layers, std::vector<std::string>& warnings, int depth = 0)
{
    if (!layers.is_array())
        return;
    for (auto layer = layers.begin(); layer != layers.end();) {
        const std::string what = "layer " + (layer->is_object() ? layer->value("name", std::string("?")) : "?");
        if (layer->is_object()) {
            if (auto clips = layer->find("clips"); clips != layer->end() && clips->is_object())
                for (auto clip = clips->begin(); clip != clips->end();) {
                    if (repair<ClipRef>(clip.value(), what + " cell " + clip.key(), warnings)) {
                        ++clip;
                        continue;
                    }
                    warnings.push_back(what + ": cell " + clip.key() + " is not understood; left empty");
                    clip = clips->erase(clip);
                }
            if (auto children = layer->find("layers"); children != layer->end() && depth == 0)
                repairLayers(*children, warnings, depth + 1);
        }
        if (repair<Layer>(*layer, what, warnings)) {
            ++layer;
            continue;
        }
        warnings.push_back(what + " is not understood; dropped");
        layer = layers.erase(layer);
    }
}

bool hasValue(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && !it->is_null();
}

// A deck switches only at a time of day: a musical Cue that names a deck is kept but switched off.
void switchOffMusicalDeckCues(nlohmann::json& cues, std::vector<std::string>& warnings)
{
    if (!cues.is_array())
        return;
    for (nlohmann::json& cue : cues) {
        if (!cue.is_object() || !hasValue(cue, "deck") || hasValue(cue, "at"))
            continue;
        cue["enabled"] = false;
        const nlohmann::json name = cue.value("name", nlohmann::json(""));
        warnings.push_back("cue " + cue.value("id", nlohmann::json()).dump() + " " + name.dump() +
                           ": a deck switches only at a time of day; loaded switched off");
    }
}

} // namespace

nlohmann::json migrateCompositionFile(const nlohmann::json& j, std::vector<std::string>& warnings)
{
    if (!j.is_object())
        throw ProtocolError("not a Nocturne composition file");
    nlohmann::json file = j;
    // The composition document alone, as a client snapshot carries it.
    if (!file.contains("composition") && file.contains("layers"))
        file = {{"format", kFormat}, {"composition", j}};
    if (!knownFormat(file)) {
        if (!file.contains("composition"))
            throw ProtocolError("not a Nocturne composition file");
        warnings.push_back("no format tag; read as a Nocturne composition");
        file["format"] = kFormat;
    }
    const nlohmann::json version = file.value("version", nlohmann::json());
    if (!version.is_number_integer() || version.get<int>() < 1) {
        warnings.push_back("no format version; migrated to version " + std::to_string(kCompositionFileVersion));
    } else if (version.get<int>() > kCompositionFileVersion) {
        warnings.push_back("saved by a newer version (format " + version.dump() + "); loaded what version " +
                           std::to_string(kCompositionFileVersion) + " understands");
    }
    file["version"] = kCompositionFileVersion;
    nlohmann::json& composition = file["composition"];
    if (!composition.is_object() || !composition.contains("layers") || !composition["layers"].is_array())
        throw ProtocolError("bad composition file: no layers");
    repairLayers(composition["layers"], warnings);
    if (composition.contains("cues"))
        switchOffMusicalDeckCues(composition["cues"], warnings);
    if (!repair<Composition>(composition, "composition", warnings))
        throw ProtocolError("bad composition file: it does not parse");
    try {
        (void)compositionFileFromJson(file);
    } catch (const ProtocolError&) {
        if (!file.contains("resume"))
            throw;
        warnings.push_back("where the show was is not understood; starting from the top");
        file.erase("resume");
    }
    return file;
}

CompositionFile compositionFileFromJson(const nlohmann::json& j, std::vector<std::string>& warnings)
{
    return compositionFileFromJson(migrateCompositionFile(j, warnings));
}

namespace {

void forEachCellJson(nlohmann::json& layers, const CellJsonVisitor& f, int depth)
{
    if (!layers.is_array() || depth > 1)
        return;
    for (nlohmann::json& layer : layers) {
        if (!layer.is_object())
            continue;
        const uint64_t id = layer.value("id", uint64_t(0));
        if (auto clips = layer.find("clips"); clips != layer.end() && clips->is_object())
            for (auto& [column, clip] : clips->items())
                f(id, std::strtoull(column.c_str(), nullptr, 10), clip);
        if (auto children = layer.find("layers"); children != layer.end())
            forEachCellJson(*children, f, depth + 1);
    }
}

} // namespace

void forEachCellJson(nlohmann::json& composition, const CellJsonVisitor& f)
{
    if (auto layers = composition.find("layers"); layers != composition.end())
        forEachCellJson(*layers, f, 0);
}

double unixTime()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace azl
