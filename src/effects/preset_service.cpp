#include "effects/preset_service.hpp"

#include "effects/classic_presets.hpp"
#include "effects/effect_registry.hpp"
#include "protocol/messages.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>

namespace azl {

namespace {

constexpr int kFileVersion = 1;

template <typename T>
T field(const Message& message, const char* key)
{
    auto it = message.find(key);
    if (it == message.end())
        throw ProtocolError(std::string("missing field ") + key);
    try {
        return it->get<T>();
    } catch (const nlohmann::json::exception&) {
        throw ProtocolError(std::string("bad field ") + key);
    }
}

Effect effectAt(const Composition& composition, const Message& message)
{
    const auto effectId = field<uint64_t>(message, "effect");
    const Effect* found = effectById(composition, effectId);
    if (!found)
        throw ProtocolError("no effect " + std::to_string(effectId));
    return *found;
}

nlohmann::json objectAt(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_object() ? *it : nlohmann::json::object();
}

// The saved presets, defaults and chains of a file in the Presets.json format, each an object.
nlohmann::json load(const std::filesystem::path& file)
{
    const nlohmann::json none{{"presets", nlohmann::json::object()}, {"defaults", nlohmann::json::object()},
                              {"chains", nlohmann::json::object()}};
    std::ifstream in(file);
    if (!in)
        return none;
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("presets") || !j["presets"].is_object()) {
        std::fprintf(stderr, "presets: ignoring unreadable %s\n", file.c_str());
        return none;
    }
    return {{"presets", j["presets"]}, {"defaults", objectAt(j, "defaults")}, {"chains", objectAt(j, "chains")}};
}

void checkEffects(const nlohmann::json& effects)
{
    if (!effects.is_array())
        throw ProtocolError("bad field effects");
    for (const nlohmann::json& e : effects)
        if (!e.is_object() || !e.contains("type") || !findEffect(e.value("type", "")))
            throw ProtocolError("a chain preset holds only known effects");
}

} // namespace

PresetService::PresetService(DocumentServer& document, std::filesystem::path file)
    : document_(document), file_(std::move(file))
{
    const nlohmann::json saved = load(file_);
    presets_ = saved["presets"];
    defaults_ = saved["defaults"];
    chains_ = saved["chains"];
    document_.handle("preset", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

PresetService::~PresetService()
{
    document_.handle("preset", nullptr);
}

std::filesystem::path PresetService::defaultFile()
{
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "/tmp") / "Library/Application Support/Nocturne/Presets.json";
}

void PresetService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    try {
        const std::string op = message.value("op", "");
        if (op == "save")
            reply(save(message));
        else if (op == "list")
            reply(list(message));
        else if (op == "apply")
            apply(message);
        else if (op == "import")
            reply(importClassic(message));
        else if (op == "rename")
            reply(rename(message));
        else if (op == "delete")
            reply(remove(message));
        else if (op == "default")
            reply(setDefault(message));
        else if (op == "saveChain" || op == "renameChain" || op == "deleteChain")
            reply(chain(message));
        else if (op == "merge")
            reply(merge(message));
        else
            throw ProtocolError("unknown preset op " + op);
    } catch (const ProtocolError& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

Message PresetService::save(const Message& message)
{
    const Effect effect = effectAt(document_.document(), message);
    const auto name = field<std::string>(message, "name");
    if (name.empty())
        throw ProtocolError("a preset needs a name");
    std::lock_guard lock(mutex_);
    presets_[effect.type][name] = effect.params;
    write();
    return {{"type", "preset"}, {"op", "save"}, {"effectType", effect.type}, {"name", name}, {"params", effect.params}};
}

Message PresetService::setDefault(const Message& message)
{
    const auto type = field<std::string>(message, "effectType");
    const auto name = field<std::string>(message, "name");
    std::lock_guard lock(mutex_);
    if (!name.empty() && !all().value(type, nlohmann::json::object()).contains(name))
        throw ProtocolError("no " + type + " preset " + name);
    if (name.empty())
        defaults_.erase(type);
    else
        defaults_[type] = name;
    write();
    return {{"type", "preset"}, {"op", "default"}, {"effectType", type}, {"name", name}};
}

Message PresetService::chain(const Message& message)
{
    const std::string op = message.value("op", "");
    const auto name = field<std::string>(message, "name");
    if (name.empty())
        throw ProtocolError("a chain preset needs a name");
    std::lock_guard lock(mutex_);
    Message echo{{"type", "preset"}, {"op", op}, {"name", name}};
    if (op == "saveChain") {
        const auto effects = field<nlohmann::json>(message, "effects");
        checkEffects(effects);
        chains_[name] = effects;
    } else {
        if (!chains_.contains(name))
            throw ProtocolError("no chain preset " + name);
        if (op == "renameChain") {
            const auto to = field<std::string>(message, "to");
            if (to.empty() || (to != name && chains_.contains(to)))
                throw ProtocolError("a chain preset needs a name no other has");
            nlohmann::json effects = chains_[name];
            chains_.erase(name);
            chains_[to] = std::move(effects);
            echo["to"] = to;
        } else {
            chains_.erase(name);
        }
    }
    write();
    return echo;
}

Message PresetService::merge(const Message& message)
{
    const auto file = field<nlohmann::json>(message, "file");
    if (!file.is_object() || !file.contains("presets") || !file["presets"].is_object())
        throw ProtocolError("not a presets file");
    const nlohmann::json defaults = objectAt(file, "defaults"), chains = objectAt(file, "chains");
    for (const auto& [name, effects] : chains.items())
        checkEffects(effects);
    std::lock_guard lock(mutex_);
    for (const auto& [type, named] : file["presets"].items())
        if (named.is_object())
            for (const auto& [name, params] : named.items())
                presets_[type][name] = params;
    for (const auto& [type, name] : defaults.items())
        defaults_[type] = name;
    for (const auto& [name, effects] : chains.items())
        chains_[name] = effects;
    write();
    return {{"type", "preset"}, {"op", "merge"}};
}

Message PresetService::list(const Message& message)
{
    std::lock_guard lock(mutex_);
    nlohmann::json presets = all();
    if (message.contains("effectType")) {
        const auto type = field<std::string>(message, "effectType");
        presets = {{type, presets.value(type, nlohmann::json::object())}};
    }
    nlohmann::json entries = nlohmann::json::array();
    const nlohmann::json byGroup = groups();
    for (const auto& [group, byType] : byGroup.items())
        for (const auto& [type, named] : byType.items())
            if (presets.contains(type))
                for (const auto& [name, params] : named.items())
                    entries.push_back({{"effectType", type}, {"name", name}, {"group", group}, {"params", params}});
    return {{"type", "preset"}, {"op", "list"},       {"presets", presets},
            {"entries", entries}, {"defaults", defaults_}, {"chains", chains_}};
}

Message PresetService::rename(const Message& message)
{
    const auto type = field<std::string>(message, "effectType");
    const auto name = field<std::string>(message, "name");
    const auto to = field<std::string>(message, "to");
    if (to.empty())
        throw ProtocolError("a preset needs a name");
    std::lock_guard lock(mutex_);
    if (!presets_.contains(type) || !presets_[type].contains(name))
        throw ProtocolError("no saved " + type + " preset " + name);
    if (to != name && presets_[type].contains(to))
        throw ProtocolError("a saved " + type + " preset " + to + " exists already");
    nlohmann::json params = presets_[type][name];
    presets_[type].erase(name);
    presets_[type][to] = std::move(params);
    write();
    return {{"type", "preset"}, {"op", "rename"}, {"effectType", type}, {"name", name}, {"to", to}};
}

Message PresetService::remove(const Message& message)
{
    const auto type = field<std::string>(message, "effectType");
    const auto name = field<std::string>(message, "name");
    std::lock_guard lock(mutex_);
    if (!presets_.contains(type) || !presets_[type].contains(name))
        throw ProtocolError("no saved " + type + " preset " + name);
    presets_[type].erase(name);
    if (presets_[type].empty())
        presets_.erase(type);
    write();
    return {{"type", "preset"}, {"op", "delete"}, {"effectType", type}, {"name", name}};
}

// Only parameters the effect still has are applied, so a preset outlives a parameter that was dropped since.
void PresetService::apply(const Message& message)
{
    const Effect effect = effectAt(document_.document(), message);
    const auto name = field<std::string>(message, "name");
    const std::string group = message.value("group", "");
    SetEffectParams edit{effect.id, {}, name};
    {
        std::lock_guard lock(mutex_);
        const nlohmann::json presets = group.empty() ? all() : groups().value(group, nlohmann::json::object());
        if (!presets.contains(effect.type) || !presets[effect.type].contains(name))
            throw ProtocolError("no " + effect.type + " preset " + name);
        const EffectDef* def = findEffect(effect.type);
        for (const ParamDef& param : def ? def->params : std::vector<ParamDef>{}) {
            const auto value = presets[effect.type].at(name).find(param.name);
            if (value != presets[effect.type].at(name).end() && value->is_number())
                edit.params[param.name] = param.wraps ? fitParam(param, value->get<double>()) : value->get<double>();
        }
    }
    document_.receive(field<ClientId>(message, "client"), msg::command(edit));
}

Message PresetService::importClassic(const Message& message)
{
    std::vector<std::filesystem::path> roots;
    if (message.contains("roots"))
        for (const auto& root : field<std::vector<std::string>>(message, "roots"))
            roots.emplace_back(root);
    else
        roots = classicPresetRoots();
    nlohmann::json imported = nlohmann::json::object(), skipped = nlohmann::json::object();
    for (const ClassicPreset& preset : readClassicPresets(roots)) {
        imported[preset.effectType][preset.name] = preset.params;
        if (!preset.skipped.empty())
            skipped[preset.effectType][preset.name] = preset.skipped;
    }
    std::lock_guard lock(mutex_);
    imported_ = imported;
    return {{"type", "preset"}, {"op", "import"}, {"presets", imported}, {"skipped", skipped}};
}

// The built-in looks, the imported presets over them and the saved ones over both.
nlohmann::json PresetService::all() const
{
    nlohmann::json presets = nlohmann::json::object();
    const nlohmann::json byGroup = groups();
    for (const char* group : {"builtin", "imported", "mine"})
        for (const auto& [type, named] : byGroup[group].items())
            for (const auto& [name, params] : named.items())
                presets[type][name] = params;
    return presets;
}

// Group -> effect type -> preset name -> parameter values, in the order the groups list.
nlohmann::json PresetService::groups() const
{
    nlohmann::json builtin = nlohmann::json::object();
    for (const ClassicPreset& look : builtinPresets())
        builtin[look.effectType][look.name] = look.params;
    nlohmann::json groups = nlohmann::json::object();
    groups["builtin"] = builtin;
    groups["imported"] = imported_;
    groups["mine"] = presets_;
    return groups;
}

void PresetService::write() const
{
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    const std::filesystem::path temp = file_.string() + ".tmp";
    {
        std::ofstream out(temp);
        out << nlohmann::json{{"version", kFileVersion}, {"presets", presets_}, {"defaults", defaults_}, {"chains", chains_}}
                   .dump(2);
        if (!out)
            throw ProtocolError("cannot write presets to " + temp.string());
    }
    std::filesystem::rename(temp, file_, ec);
    if (ec)
        throw ProtocolError("cannot write presets to " + file_.string() + ": " + ec.message());
}

} // namespace azl
