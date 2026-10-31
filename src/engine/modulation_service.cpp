#include "engine/modulation_service.hpp"

#include "document/modulation_edits.hpp"
#include "engine/macro_sets_file.hpp"
#include "platform/app_paths.hpp"
#include "protocol/messages.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

namespace azl {

namespace {

constexpr int kFileVersion = 2;

std::string nameField(const Message& message)
{
    auto it = message.find("name");
    if (it == message.end() || !it->is_string() || it->get<std::string>().empty())
        throw ProtocolError("a Macro Set needs a name");
    return it->get<std::string>();
}

// The indices in composition.macros of the composition's macro slots, 1 to 8 in order.
std::vector<size_t> slotIndices(const Composition& composition)
{
    std::vector<size_t> slots;
    for (size_t i = 0; i < composition.macros.size() && slots.size() < kMaxMacros; ++i)
        if (composition.macros[i].group == 0)
            slots.push_back(i);
    return slots;
}

std::string count(size_t n, const std::string& one, const std::string& many)
{
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string sourceName(const Modulator& modulator)
{
    if (modulator.source == ModulatorSource::Lfo)
        return "Beat LFO";
    std::string name = modulator.feature;
    if (!name.empty())
        name[0] = char(std::toupper(name[0]));
    return name;
}

// A set's macro in its slot, with the modulators driving it.
struct SetMacro {
    std::string name;
    double value = 0.0;
    std::vector<Modulator> modulators;
};

// The set's macros by slot, and what an older set file holds that a set no longer carries.
struct ReadSet {
    std::vector<SetMacro> macros;
    std::vector<std::string> notes;
};

ReadSet readSet(const nlohmann::json& set)
{
    ReadSet read;
    std::map<uint64_t, size_t> slots;
    size_t targets = 0, groupMacros = 0, otherModulators = 0;
    for (const nlohmann::json& saved : set.value("macros", nlohmann::json::array())) {
        const Macro macro = saved.get<Macro>();
        targets += macro.targets.size();
        if (macro.group) {
            ++groupMacros;
            continue;
        }
        slots[macro.id] = read.macros.size();
        read.macros.push_back({macro.name, macro.value, {}});
    }
    for (const nlohmann::json& saved : set.value("modulators", nlohmann::json::array())) {
        Modulator modulator = saved.get<Modulator>();
        const auto slot = slots.find(modulator.target.macro);
        if (slot == slots.end()) {
            ++otherModulators;
            continue;
        }
        modulator.id = 0;
        read.macros[slot->second].modulators.push_back(modulator);
    }
    if (targets)
        read.notes.push_back("The set's " + count(targets, "target is", "targets are") +
                             " ignored: macros keep this show's targets");
    if (groupMacros)
        read.notes.push_back(count(groupMacros, "group macro", "group macros") + " of the set ignored");
    if (otherModulators)
        read.notes.push_back(count(otherModulators, "modulator", "modulators") + " on parameters or group macros ignored");
    return read;
}

// What a set brings, item by item, and the edit that loads it without the items in `drop`. Slot N's macro takes the
// set's name, value and modulators and keeps its targets; show macros beyond the set lose their modulators; set macros
// beyond the show are created without targets. A dropped macro leaves its slot as it is.
struct Plan {
    SetMacros edit;
    nlohmann::json items = nlohmann::json::array();
    nlohmann::json notes = nlohmann::json::array();
    nlohmann::json created = nlohmann::json::array();
};

uint64_t freeMacroId(const Composition& composition)
{
    uint64_t id = 0;
    for (const Macro& macro : composition.macros)
        id = std::max(id, macro.id);
    return id + 1;
}

Plan planLoad(const Composition& composition, const nlohmann::json& set, const std::set<std::string>& drop)
{
    const ReadSet read = readSet(set);
    const std::vector<size_t> slots = slotIndices(composition);
    Plan plan;
    plan.notes = read.notes;
    plan.edit.macros = composition.macros;
    std::set<uint64_t> replaced;
    std::vector<Modulator> loaded;
    uint64_t nextId = freeMacroId(composition);
    size_t modulatorKey = 0;
    for (size_t i = 0; i < read.macros.size(); ++i) {
        const SetMacro& saved = read.macros[i];
        const std::string key = "m" + std::to_string(i);
        const bool fresh = i >= slots.size();
        plan.items.push_back({{"key", key}, {"kind", "macro"}, {"label", saved.name}, {"status", fresh ? "new" : "ok"}});
        const bool kept = !drop.count(key);
        uint64_t id = 0;
        if (kept && fresh) {
            id = nextId++;
            plan.edit.macros.push_back({.id = id, .name = saved.name, .value = saved.value});
            plan.created.push_back(saved.name);
        } else if (kept) {
            Macro& macro = plan.edit.macros[slots[i]];
            macro.name = saved.name;
            macro.value = saved.value;
            id = macro.id;
            replaced.insert(id);
        }
        for (Modulator modulator : saved.modulators) {
            const std::string modKey = "d" + std::to_string(modulatorKey++);
            plan.items.push_back({{"key", modKey}, {"kind", "modulator"}, {"label", sourceName(modulator) + " -> " + saved.name},
                                  {"status", "ok"}, {"parent", key}});
            if (!kept || drop.count(modKey))
                continue;
            modulator.target.macro = id;
            loaded.push_back(modulator);
        }
    }
    for (size_t i = read.macros.size(); i < slots.size(); ++i) {
        const Macro& macro = composition.macros[slots[i]];
        replaced.insert(macro.id);
        plan.notes.push_back(macro.name + " is not in the set: it keeps its targets, without modulators");
    }
    for (const Modulator& modulator : composition.modulators)
        if (!replaced.count(modulator.target.macro))
            plan.edit.modulators.push_back(modulator);
    plan.edit.modulators.insert(plan.edit.modulators.end(), loaded.begin(), loaded.end());
    return plan;
}

Plan planFor(const std::string& name, const nlohmann::json& set, const Composition& composition,
             const std::set<std::string>& drop)
{
    try {
        return planLoad(composition, set, drop);
    } catch (const nlohmann::json::exception& e) {
        throw ProtocolError("Macro Set " + name + " is unreadable: " + e.what());
    }
}

nlohmann::json loadFile(const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in)
        return nlohmann::json::object();
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.contains("sets") || !j["sets"].is_object()) {
        std::fprintf(stderr, "macro sets: ignoring unreadable %s\n", file.c_str());
        return nlohmann::json::object();
    }
    return j["sets"];
}

} // namespace

ModulationService::ModulationService(DocumentServer& document, Engine& engine, std::filesystem::path file)
    : document_(document), engine_(engine), file_(std::move(file)), sets_(loadFile(file_))
{
    document_.handle("modulation",
                     [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

ModulationService::~ModulationService()
{
    document_.handle("modulation", nullptr);
}

std::filesystem::path ModulationService::defaultFile()
{
    return defaultMacroSetsFile();
}

void ModulationService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    try {
        const std::string op = message.value("op", "");
        if (op == "state")
            reply(state());
        else if (op == "save")
            reply(save(message));
        else if (op == "list")
            reply(list());
        else if (op == "load")
            load(message, reply);
        else if (op == "preview")
            reply(preview(message));
        else
            throw ProtocolError("unknown modulation op " + op);
    } catch (const ProtocolError& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

Message ModulationService::state() const
{
    const std::shared_ptr<const ModulationPlan> plan = engine_.modulation();
    nlohmann::json macros = nlohmann::json::array();
    for (const auto& [id, value] : plan->macros())
        macros.push_back({{"id", id}, {"value", value}});
    nlohmann::json modulators = nlohmann::json::array();
    for (const auto& [id, value] : plan->modulators())
        modulators.push_back({{"id", id}, {"value", value}});
    for (const auto& [id, low, high] : plan->ranges())
        for (nlohmann::json& m : modulators)
            if (m["id"] == id)
                m["range"] = {low, high};
    nlohmann::json params = nlohmann::json::array();
    for (const auto& [param, value] : plan->params()) {
        nlohmann::json p = param;
        p["value"] = value;
        params.push_back(p);
    }
    return {{"type", "modulation"}, {"op", "state"}, {"macros", macros}, {"modulators", modulators}, {"params", params},
            {"dangling", plan->dangling()}};
}

Message ModulationService::save(const Message& message)
{
    const std::string name = nameField(message);
    const Composition composition = document_.document();
    nlohmann::json macros = nlohmann::json::array();
    std::map<uint64_t, size_t> slots;
    for (size_t index : slotIndices(composition)) {
        const Macro& macro = composition.macros[index];
        slots[macro.id] = macros.size() + 1;
        macros.push_back({{"id", slots[macro.id]}, {"name", macro.name}, {"value", macro.value}});
    }
    nlohmann::json modulators = nlohmann::json::array();
    for (const Modulator& modulator : composition.modulators) {
        const auto slot = slots.find(modulator.target.macro);
        if (slot == slots.end())
            continue;
        nlohmann::json saved = modulator;
        saved.erase("id");
        saved["target"] = {{"macro", slot->second}};
        modulators.push_back(saved);
    }
    std::lock_guard lock(mutex_);
    sets_[name] = {{"macros", macros}, {"modulators", modulators}};
    write();
    return {{"type", "modulation"}, {"op", "save"}, {"name", name}, {"macros", macros.size()},
            {"modulators", modulators.size()}};
}

Message ModulationService::list()
{
    std::lock_guard lock(mutex_);
    return {{"type", "modulation"}, {"op", "list"}, {"sets", sets_}};
}

nlohmann::json ModulationService::setNamed(const std::string& name)
{
    std::lock_guard lock(mutex_);
    if (!sets_.contains(name))
        throw ProtocolError("no Macro Set " + name);
    return sets_[name];
}

Message ModulationService::preview(const Message& message)
{
    const std::string name = nameField(message);
    const Composition composition = document_.document();
    const Plan plan = planFor(name, setNamed(name), composition, {});
    const std::vector<size_t> slots = slotIndices(composition);
    std::set<uint64_t> ids;
    for (size_t index : slots)
        ids.insert(composition.macros[index].id);
    const auto driving = std::count_if(composition.modulators.begin(), composition.modulators.end(),
                                       [&](const Modulator& modulator) { return ids.count(modulator.target.macro); });
    return {{"type", "modulation"}, {"op", "preview"}, {"name", name}, {"items", plan.items}, {"notes", plan.notes},
            {"replaces", {{"macros", slots.size()}, {"modulators", driving}}}};
}

void ModulationService::load(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string name = nameField(message);
    const Plan plan = planFor(name, setNamed(name), document_.document(), message.value("drop", std::set<std::string>{}));
    reply({{"type", "modulation"}, {"op", "load"}, {"name", name}, {"created", plan.created}, {"notes", plan.notes}});
    document_.receive(message.at("client").get<ClientId>(), msg::command(plan.edit));
}

void ModulationService::write() const
{
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    const std::filesystem::path temp = file_.string() + ".tmp";
    {
        std::ofstream out(temp);
        out << nlohmann::json{{"version", kFileVersion}, {"sets", sets_}}.dump(2);
        if (!out)
            throw ProtocolError("cannot write Macro Sets to " + temp.string());
    }
    std::filesystem::rename(temp, file_, ec);
    if (ec)
        throw ProtocolError("cannot write Macro Sets to " + file_.string() + ": " + ec.message());
}

} // namespace azl
