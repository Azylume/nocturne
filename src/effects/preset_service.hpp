#pragma once

#include "protocol/document_server.hpp"

#include <filesystem>
#include <mutex>

namespace azl {

// Effect presets: named parameter sets per effect type, kept per machine in one JSON file. They are not part of the
// composition, so they are not replicated; applying one is an ordinary undoable edit. Served as "preset" messages:
//
//   {"type": "preset", "op": "save", "effect": E, "name": N}
//     -> {"type": "preset", "op": "save", "effectType": T, "name": N, "params": {...}}   (overwrites a same-named one)
//   {"type": "preset", "op": "list"} or with "effectType": T
//     -> {"type": "preset", "op": "list", "presets": {T: {N: {...}}},
//         "entries": [{"effectType": T, "name": N, "group": "builtin" | "imported" | "mine", "params": {...}}],
//         "defaults": {T: N}, "chains": {N: [effect, ...]}}
//   {"type": "preset", "op": "apply", "effect": E, "name": N[, "group": G]}
//     -> a setEffectParams change naming the preset, one undo step
//   {"type": "preset", "op": "rename", "effectType": T, "name": N, "to": M}
//   {"type": "preset", "op": "delete", "effectType": T, "name": N}
//     -> the request echoed; both act on saved presets only
//   {"type": "preset", "op": "default", "effectType": T, "name": N}
//     -> the request echoed: new effects of type T start from preset N (an empty name: from the type's defaults)
//   {"type": "preset", "op": "saveChain", "name": N, "effects": [effect, ...]}
//   {"type": "preset", "op": "renameChain", "name": N, "to": M}
//   {"type": "preset", "op": "deleteChain", "name": N}
//     -> the request echoed; a chain preset is a named list of effects (type, params, bypass, preset)
//   {"type": "preset", "op": "merge", "file": {"presets": ..., "defaults": ..., "chains": ...}}
//     -> the request echoed; a file in the Presets.json format joins the saved presets, its entries winning
//   {"type": "preset", "op": "import"} or with "roots": [DIR...]
//     -> {"type": "preset", "op": "import", "presets": {T: {N: {...}}}, "skipped": {T: {N: [what was not taken]}}}
//
// Import reads the Classic pack's presets from the other app's preset folders on this machine (classic_presets.hpp)
// into memory only; they list and apply like saved ones, and a saved preset of the same name wins. "presets" merges the
// groups (built-in looks, then imported, then saved, the later winning); "entries" lists every preset of every group.
//
// An older message's "layer" is ignored. Errors come back as {"type": "error"}; replies go to the sender only.
class PresetService {
public:
    PresetService(DocumentServer& document, std::filesystem::path file);
    ~PresetService();

    // ~/Library/Application Support/Nocturne/Presets.json
    static std::filesystem::path defaultFile();

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    Message save(const Message& message);
    Message list(const Message& message);
    void apply(const Message& message);
    Message rename(const Message& message);
    Message remove(const Message& message);
    Message setDefault(const Message& message);
    Message chain(const Message& message);
    Message merge(const Message& message);
    Message importClassic(const Message& message);
    nlohmann::json all() const;
    nlohmann::json groups() const;
    void write() const;

    DocumentServer& document_;
    std::filesystem::path file_;
    std::mutex mutex_;
    // Effect type -> preset name -> parameter values.
    nlohmann::json presets_ = nlohmann::json::object();
    nlohmann::json imported_ = nlohmann::json::object();
    // Effect type -> the preset new effects of it start from.
    nlohmann::json defaults_ = nlohmann::json::object();
    // Chain preset name -> its effects.
    nlohmann::json chains_ = nlohmann::json::object();
};

} // namespace azl
