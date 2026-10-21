#pragma once

#include "engine/isf_library.hpp"
#include "protocol/document_server.hpp"

#include <functional>

namespace azl {

// The user folder's ISF shaders over the protocol, as "isf" messages:
//
//   {"type": "isf", "op": "list"[, "text": true]}
//     -> {"type": "isf", "op": "list", "folder": F,
//         "shaders": [{"file": N, "name": S, "type": T, "kind": "effect" | "generator" | "transition", "error": E,
//                      "text": source (with "text": true)}]}
//   {"type": "isf", "op": "rescan"[, "text": true]}
//     -> the same with "op": "rescan", after reading the folder again
//
// After a rescan that changed anything every client gets the list with the sources, which a client registers to show
// the shaders and their parameters (mirrorUserIsf). The sources go to the engine's clients only. Errors come back as
// {"type": "error"}.
class IsfService {
public:
    // `changed` runs after a rescan that registered, changed or retired a shader, so the engine recompiles its chains.
    IsfService(DocumentServer& document, UserIsfLibrary& library, std::function<void()> changed);
    ~IsfService();

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    Message list(const char* op, bool text) const;

    DocumentServer& document_;
    UserIsfLibrary& library_;
    std::function<void()> changed_;
};

} // namespace azl
