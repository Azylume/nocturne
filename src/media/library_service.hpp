#pragma once

#include "media/media_library.hpp"
#include "platform/work_queue.hpp"
#include "protocol/document_server.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace azl {

inline constexpr uint32_t kStillWidth = 320;

struct StillCounts {
    // "still" requests answered.
    std::atomic<int> asked{0};
    // Stills rendered from video, not served from the cache.
    std::atomic<int> rendered{0};
};

// Serves the machine's Media Library over the protocol as "library" messages. They are machine-local, not document
// commands: the library belongs to the machine, is not undoable and is not replicated to nodes; the composition only
// stores the hash and display name a client then puts into a cell with setSlot (or setLayerClip).
//
//   {"type": "library", "op": "add", "path": "/absolute/path/on/the/engine/machine.mp4"}
//     -> {"type": "library", "op": "add", "entry": {hash, name, path, status, reason, ...}}
//   {"type": "library", "op": "list"} -> {"type": "library", "op": "list", "entries": [...]}
//   {"type": "library", "op": "relink", "hash": H[, "path": P]}
//     -> {"type": "library", "op": "relink", "hash": H, "entry": {...}}: finds missing media again, by hash in the
//     library's folders, or as the file at P (added to the library). Clips of that hash retry at once. When P holds
//     other content, the entry's hash differs and the client replaces H in its cells (setSlot, one gesture).
//   {"type": "library", "op": "thumbnail", "hash": H, "width": 160}
//     -> {"type": "library", "op": "thumbnail", "hash": H, "width": 160, "jpeg": <binary>} (or "error": reason)
//     a still a third into the clip, cached in the library folder
//   {"type": "library", "op": "still", "hash": H}
//     -> {"type": "library", "op": "still", "hash": H, "jpeg": <binary>} (or "error": reason)
//     the one still per clip a Director keeps for media it lacks: kStillWidth wide, rendered once and cached as above
//
// Requests run in order on a background queue (adding hashes the whole file), and each is answered to its sender only.
class LibraryService {
public:
    // `found` runs with each hash an add or relink makes playable, so clips that could not open it try again.
    LibraryService(DocumentServer& document, MediaLibrary& library, std::function<void(const std::string&)> found = {});
    // Waits for the request in progress.
    ~LibraryService();

    // Since the start.
    const StillCounts& stills() const { return stills_; }

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);

    DocumentServer& document_;
    MediaLibrary& library_;
    std::function<void(const std::string&)> found_;
    WorkQueue queue_;
    StillCounts stills_;
};

} // namespace azl
