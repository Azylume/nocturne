#include "media/composition_media.hpp"

#include "document/modulation_edits.hpp"
#include "effects/effect_registry.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace azl {

namespace fs = std::filesystem;

namespace {

struct MediaRef {
    std::string path;
    uint64_t size = 0;
};

using CellKey = std::pair<uint64_t, uint64_t>;

void startCellsWhenLoaded(Composition& composition)
{
    composition.timeline = {};
    forEachLayer(composition.layers, [](Layer& layer) {
        if (layer.playing)
            layer.playing->beat.reset();
    });
}

std::string portablePath(const fs::path& file, const fs::path& root)
{
    const fs::path relative = file.lexically_normal().lexically_relative(root.lexically_normal());
    if (!relative.empty() && *relative.begin() != "..")
        return relative.generic_string();
    return file.string();
}

bool isFile(const fs::path& path)
{
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// Finds a clip's file and returns its hash, adding it to the library. Results are kept per clip, so a clip in many
// cells is looked for once.
class Resolver {
public:
    Resolver(MediaLibrary& library, fs::path root, std::vector<std::string>& warnings)
        : library_(library), root_(std::move(root)), warnings_(warnings)
    {
    }

    std::optional<std::string> resolve(const ClipRef& clip, const MediaRef& ref)
    {
        const std::string key = clip.hash + "\n" + clip.name + "\n" + ref.path;
        if (auto done = done_.find(key); done != done_.end())
            return done->second;
        return done_[key] = find(clip, ref);
    }

private:
    std::optional<std::string> find(const ClipRef& clip, const MediaRef& ref)
    {
        if (!ref.path.empty()) {
            const fs::path path = fs::path(ref.path).is_absolute() ? fs::path(ref.path) : root_ / ref.path;
            if (auto hash = add(path, clip.hash)) {
                if (*hash != clip.hash)
                    warn(clip, "changed since it was saved; playing it as it is now");
                return hash;
            }
        }
        if (auto path = byName(clip.name)) {
            if (auto hash = add(*path, clip.hash)) {
                warn(clip, "found by name at " + path->string());
                return hash;
            }
        }
        if (auto entry = library_.find(clip.hash); entry && entry->status != MediaStatus::Missing) {
            warn(clip, "found by content at " + entry->path.string());
            return clip.hash;
        }
        if (auto path = bySize(ref.size, clip.hash)) {
            warn(clip, "found by content at " + path->string());
            return add(*path, clip.hash);
        }
        warn(clip, "missing" + (ref.path.empty() ? std::string() : " (" + ref.path + ")") + "; its cell is empty");
        return std::nullopt;
    }

    // A file the library already knows with that content and stamp is not hashed again.
    std::optional<std::string> add(const fs::path& path, const std::string& expected)
    {
        if (!isFile(path))
            return std::nullopt;
        const fs::path normal = fs::absolute(path).lexically_normal();
        if (auto known = library_.find(expected); known && known->status != MediaStatus::Missing && known->path == normal)
            return expected;
        try {
            return library_.add(normal).hash;
        } catch (const std::exception& e) {
            warnings_.push_back(e.what());
            return std::nullopt;
        }
    }

    void scan()
    {
        if (scanned_)
            return;
        scanned_ = true;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc))
                continue;
            files_.push_back({it->path(), it->file_size(fileEc)});
        }
        std::sort(files_.begin(), files_.end());
    }

    std::optional<fs::path> byName(const std::string& name)
    {
        if (name.empty())
            return std::nullopt;
        scan();
        for (const auto& [path, size] : files_)
            if (path.filename() == name)
                return path;
        return std::nullopt;
    }

    std::optional<fs::path> bySize(uint64_t size, const std::string& hash)
    {
        if (size == 0)
            return std::nullopt;
        scan();
        for (const auto& [path, fileSize] : files_) {
            if (fileSize != size)
                continue;
            try {
                if (sha256File(path) == hash)
                    return path;
            } catch (const std::exception&) {
            }
        }
        return std::nullopt;
    }

    void warn(const ClipRef& clip, const std::string& what) { warnings_.push_back("media " + clip.name + ": " + what); }

    MediaLibrary& library_;
    fs::path root_;
    std::vector<std::string>& warnings_;
    std::map<std::string, std::optional<std::string>> done_;
    bool scanned_ = false;
    std::vector<std::pair<fs::path, uint64_t>> files_;
};

std::string layerName(const Layer& layer)
{
    return layer.name.empty() ? "layer " + std::to_string(layer.id) : layer.name;
}

void completeParams(const EffectDef& def, Effect& effect, const std::string& where, std::vector<std::string>& warnings)
{
    std::map<std::string, double> params;
    for (const ParamDef& p : def.params) {
        auto given = effect.params.find(p.name);
        params[p.name] = given == effect.params.end() ? p.defaultValue : fitParam(p, given->second);
        if (given != effect.params.end() && !p.wraps && params[p.name] != given->second)
            warnings.push_back(where + ": " + effect.type + "." + p.name + " " + std::to_string(given->second) +
                               " is outside " + std::to_string(p.min) + ".." + std::to_string(p.max) + "; clamped");
    }
    for (const auto& [name, value] : effect.params)
        if (!params.contains(name))
            warnings.push_back(where + ": " + effect.type + " has no parameter " + name + "; skipped");
    effect.params = std::move(params);
}

void dropUnknown(std::vector<Effect>& effects, const std::string& where, std::vector<std::string>& warnings)
{
    std::erase_if(effects, [&](Effect& effect) {
        const EffectDef* def = findEffect(effect.type);
        if (!def) {
            warnings.push_back(where + ": unknown effect " + effect.type + "; skipped");
            return true;
        }
        completeParams(*def, effect, where, warnings);
        return false;
    });
}

} // namespace

fs::path mediaRootOf(const fs::path& file, const fs::path& mediaRoot)
{
    return fs::absolute(mediaRoot.empty() ? file.parent_path() : mediaRoot).lexically_normal();
}

std::vector<std::string> saveComposition(const fs::path& file, const Composition& composition, MediaLibrary& library,
                                         const fs::path& mediaRoot)
{
    const fs::path root = mediaRootOf(file, mediaRoot);
    CompositionFile saved{composition};
    startCellsWhenLoaded(saved.composition);
    nlohmann::json j = toJson(saved);
    j["mediaRoot"] = root.string();
    std::vector<std::string> warnings;
    std::map<std::string, std::optional<MediaEntry>> entries;
    forEachCellJson(j["composition"], [&](uint64_t, uint64_t, nlohmann::json& clip) {
        const std::string hash = clip.value("hash", "");
        if (clip.contains("text") || hash.empty())
            return;
        if (!entries.contains(hash))
            entries[hash] = library.find(hash);
        const std::optional<MediaEntry>& entry = entries[hash];
        if (!entry || entry->status == MediaStatus::Missing) {
            warnings.push_back("media " + clip.value("name", hash) + ": not in the media library; saved by hash only");
            return;
        }
        clip["path"] = portablePath(entry->path, root);
        clip["size"] = entry->size;
    });
    writeFileAtomically(file, j.dump(2) + "\n");
    return warnings;
}

LoadedComposition loadComposition(const fs::path& file, MediaLibrary& library,
                                  const DisplayPlacement& displays, const fs::path& mediaRoot)
{
    std::optional<nlohmann::json> j = readJsonFile(file);
    if (!j)
        throw std::runtime_error("no file " + file.string());
    LoadedComposition loaded;
    nlohmann::json migrated = migrateCompositionFile(*j, loaded.warnings);
    loaded.composition = compositionFileFromJson(migrated).composition;
    std::map<CellKey, MediaRef> refs;
    forEachCellJson(migrated["composition"], [&](uint64_t layer, uint64_t column, nlohmann::json& clip) {
        if (clip.is_object())
            refs[{layer, column}] = {clip.value("path", ""), clip.value("size", uint64_t(0))};
    });

    Composition& composition = loaded.composition;
    startCellsWhenLoaded(composition);
    Resolver resolver(library, mediaRootOf(file, mediaRoot), loaded.warnings);
    forEachLayer(composition.layers, [&](Layer& layer) {
        for (auto it = layer.clips.begin(); it != layer.clips.end();) {
            if (it->second.text || isGeneratorClip(it->second)) {
                ++it;
                continue;
            }
            const std::optional<std::string> hash = resolver.resolve(it->second, refs[{layer.id, it->first}]);
            if (!hash) {
                it = layer.clips.erase(it);
                continue;
            }
            it->second.hash = *hash;
            ++it;
        }
    });
    loaded.removed = dropRemovedEffects(composition);
    for (const std::string& line : loaded.removed)
        loaded.warnings.push_back("removed effect dropped: " + line);
    dropUnknownEffects(composition, loaded.warnings);
    moveToAttachedDisplays(composition, displays, loaded.warnings);
    return loaded;
}

namespace {

const std::map<std::string, std::string> kRemovedEffects{{"bloom", "Bloom"}, {"classicBloom", "Classic Bloom"}};

std::string named(const std::string& what, uint64_t id, const std::string& name)
{
    return what + " " + std::to_string(id) + (name.empty() ? "" : " " + name);
}

bool doesNothing(const Cue& cue)
{
    return cue.autopilot.empty() && cue.macros.empty() && !cue.column && cue.effects.empty() && !cue.deck &&
           !cue.prepared;
}

} // namespace

std::vector<std::string> dropRemovedEffects(Composition& composition)
{
    std::vector<std::string> dropped;
    std::set<uint64_t> ids;
    auto drop = [&](std::vector<Effect>& effects, const std::string& where) {
        std::erase_if(effects, [&](const Effect& effect) {
            auto removed = kRemovedEffects.find(effect.type);
            if (removed == kRemovedEffects.end())
                return false;
            ids.insert(effect.id);
            dropped.push_back(removed->second + " (effect " + std::to_string(effect.id) + ") on " + where);
            return true;
        });
    };
    drop(composition.effects, "the composition");
    forEachLayer(composition.layers, [&](Layer& layer) {
        drop(layer.effects, layerName(layer));
        for (auto& [deck, settings] : layer.decks)
            if (settings.effects)
                drop(*settings.effects, layerName(layer) + ", deck " + std::to_string(deck));
        for (auto& [column, clip] : layer.clips)
            drop(clip.effects, layerName(layer) + ", clip " + clip.name);
    });
    if (ids.empty())
        return dropped;
    for (Macro& macro : composition.macros)
        std::erase_if(macro.targets, [&](const MacroTarget& target) {
            if (!ids.contains(target.param.effect))
                return false;
            dropped.push_back("macro " + macro.name + ": its target " + target.param.param + " of effect " +
                              std::to_string(target.param.effect));
            return true;
        });
    std::erase_if(composition.modulators, [&](const Modulator& m) {
        if (m.target.macro || !ids.contains(m.target.param.effect))
            return false;
        dropped.push_back(named("modulator", m.id, m.name) + " (on effect " + std::to_string(m.target.param.effect) +
                          ")");
        return true;
    });
    std::erase_if(composition.cues, [&](Cue& cue) {
        const size_t before = cue.effects.size();
        std::erase_if(cue.effects, [&](const CueEffect& e) { return ids.contains(e.effect); });
        if (cue.effects.size() == before)
            return false;
        if (doesNothing(cue)) {
            dropped.push_back(named("cue", cue.id, cue.name));
            return true;
        }
        dropped.push_back(named("cue", cue.id, cue.name) + ": its steps of the removed effects");
        return false;
    });
    return dropped;
}

std::string removedEffectsNotice(const std::vector<std::string>& dropped)
{
    std::string notice = "Bloom and Classic Bloom are removed; dropped from this show: ";
    for (size_t i = 0; i < dropped.size(); ++i)
        notice += (i ? "; " : "") + dropped[i];
    return notice;
}

void dropUnknownEffects(Composition& composition, std::vector<std::string>& warnings)
{
    dropUnknown(composition.effects, "composition", warnings);
    forEachLayer(composition.layers, [&](Layer& layer) {
        dropUnknown(layer.effects, layerName(layer), warnings);
        for (auto& [deck, settings] : layer.decks)
            if (settings.effects)
                dropUnknown(*settings.effects, layerName(layer) + " / deck " + std::to_string(deck), warnings);
        for (auto& [column, clip] : layer.clips)
            dropUnknown(clip.effects, layerName(layer) + " / clip " + clip.name, warnings);
    });
    // Offsets reach at most one span either way.
    auto fitOffsets = [&](const std::string& what, const ParamRef& param, double& min, double& max) {
        const auto range = paramRange(composition, param);
        const double span = range ? range->second - range->first : 0.0;
        if (!range || (std::abs(min) <= span && std::abs(max) <= span))
            return;
        min = std::clamp(min, -span, span);
        max = std::clamp(max, -span, span);
        warnings.push_back(what + ": its range on " + param.param + " is wider than the parameter's; clamped");
    };
    for (Macro& macro : composition.macros)
        for (MacroTarget& target : macro.targets)
            fitOffsets("macro " + macro.name, target.param, target.min, target.max);
    for (Modulator& m : composition.modulators)
        if (!m.target.macro)
            fitOffsets("modulator " + m.name, m.target.param, m.min, m.max);
    for (Macro& macro : composition.macros)
        std::erase_if(macro.targets, [&](const MacroTarget& target) {
            if (paramRange(composition, target.param))
                return false;
            warnings.push_back("macro " + macro.name + ": its target " + target.param.param + " is gone; dropped");
            return true;
        });
    std::erase_if(composition.modulators, [&](const Modulator& m) {
        const bool drives = m.target.macro ? std::any_of(composition.macros.begin(), composition.macros.end(),
                                                         [&](const Macro& macro) { return macro.id == m.target.macro; })
                                           : paramRange(composition, m.target.param).has_value();
        if (!drives)
            warnings.push_back("modulator " + m.name + ": its target is gone; dropped");
        return !drives;
    });
}

void moveToAttachedDisplays(Composition& composition, const DisplayPlacement& displays,
                            std::vector<std::string>& warnings)
{
    placeByRole(composition, displays.roles, warnings);
    const std::vector<std::string>& attached = displays.attached;
    auto isAttached = [&](const std::string& id) { return std::find(attached.begin(), attached.end(), id) != attached.end(); };
    std::set<std::string> used;
    for (const Screen& screen : composition.screens)
        if (isAttached(screen.display))
            used.insert(screen.display);
    for (Screen& screen : composition.screens) {
        if (screen.display.empty() || isAttached(screen.display))
            continue;
        if (!displays.remap && displays.known.contains(screen.display)) {
            warnings.push_back("screen " + screen.name + ": display " + screen.display +
                               " is known here but not attached; parked until it attaches");
            continue;
        }
        auto free = std::find_if(attached.begin(), attached.end(), [&](const std::string& id) { return !used.contains(id); });
        if (free == attached.end()) {
            warnings.push_back("screen " + screen.name + ": display " + screen.display + " is not attached; parked");
            continue;
        }
        warnings.push_back("screen " + screen.name + ": display " + screen.display + " is not attached; showing on " +
                           *free);
        screen.display = *free;
        used.insert(*free);
    }
    if (composition.screens.empty() && !attached.empty()) {
        warnings.push_back("no screens; showing the composition on " + attached.front());
        composition.screens.push_back(
            {.id = 1, .name = "Screen 1", .display = attached.front(), .slices = {{.id = 1, .name = "Slice 1"}}});
    }
}

} // namespace azl
