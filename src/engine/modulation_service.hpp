#pragma once

#include "engine/engine.hpp"
#include "protocol/document_server.hpp"

#include <filesystem>
#include <mutex>

namespace azl {

// Serves the live modulation values and Macro Sets as "modulation" messages. Macros and modulators themselves are
// document state, edited with undoable commands (addMacro, setMacroValue, addModulator, ...).
//
//   {"op": "state"} -> {"op": "state", "macros": [{"id", "value"}], "modulators": [{"id", "value"}], "params":
//                       [{"layer" or "effect", "param", "value"}]}, as of the last rendered frame: each macro's value
//                       after its modulators, each modulator's source before its curve, each driven parameter;
//                       "dangling": [id], the modulators whose target parameter does not exist
//   {"op": "save", "name": N} -> {"op": "save", "name", "macros": count, "modulators": count}, the composition's
//                       macro slots (names and values, by slot 1-8) and the modulators driving them, kept as a Macro
//                       Set on this machine (overwrites a same-named one); never targets
//   {"op": "list"}  -> {"op": "list", "sets": {N: {"macros", "modulators"}}}
//   {"op": "load", "name": N} -> {"op": "load", "name", "created": [name], "notes": [...]}, then a setMacros change
//                       (one undo step): slot N's macro takes the set's name, value and modulators and keeps its
//                       targets; show macros beyond the set keep their targets without modulators; set macros beyond
//                       the show are created without targets ("created"). Group macros and modulators on parameters
//                       stay. "drop": [key] leaves out those items of the preview; a dropped macro leaves its slot as
//                       it is
//   {"op": "preview", "name": N} -> {"op": "preview", "name", "items": [{"key", "kind": "macro" or "modulator",
//                       "label", "status": "ok" or "new" (a macro created without targets), "parent": the macro's
//                       key}], "notes": [...], "replaces": {"macros", "modulators"}}: what a load brings and replaces
//
// "notes" name the show macros beyond the set, and what an older set file holds that a set no longer carries (targets,
// group macros, modulators on parameters), which a load ignores.
//
// Every message has "type": "modulation"; replies go to the sender, errors as {"type": "error"}.
class ModulationService {
public:
    ModulationService(DocumentServer& document, Engine& engine, std::filesystem::path file);
    ~ModulationService();

    // ~/Library/Application Support/Nocturne/MacroSets.json
    static std::filesystem::path defaultFile();

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    Message state() const;
    Message save(const Message& message);
    Message list();
    nlohmann::json setNamed(const std::string& name);
    Message preview(const Message& message);
    void load(const Message& message, const DocumentServer::Sink& reply);
    void write() const;

    DocumentServer& document_;
    Engine& engine_;
    std::filesystem::path file_;
    std::mutex mutex_;
    nlohmann::json sets_ = nlohmann::json::object();
};

} // namespace azl
