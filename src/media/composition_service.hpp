#pragma once

#include "media/composition_media.hpp"
#include "media/media_library.hpp"
#include "platform/work_queue.hpp"
#include "protocol/document_server.hpp"

#include <functional>
#include <string>
#include <vector>

namespace azl {

// Saves and loads the whole composition as a portable file (composition_media.hpp) on the engine machine, as
// "composition" messages. Paths are absolute on the engine machine; "mediaRoot" is optional (default: the file's
// folder).
//
//   {"op": "save", "path": P[, "mediaRoot": R]} -> {"op": "save", "path", "warnings": [...]}
//   {"op": "load", "path": P[, "mediaRoot": R][, "remapDisplays": true]} -> {"op": "load", "path", "warnings": [...]},
//                       then a loadComposition change from the sender (one undo step) that replaces the document;
//                       remapDisplays moves screens off displays this machine knows but has not attached
//   {"op": "shows"[, "folder": F]} -> {"op": "shows", "folder", "showsFolder", "recent": [P...],
//                       "shows": [{"path", "name", "modified" (Unix seconds), "size", "mediaPresent"}...]}
//                       the composition files in F, or else in the shows folder, the recent shows and their folders,
//                       newest first; only inside the allowed roots, and a folder outside them is an error. "recent"
//                       lists the shows saved or loaded last, newest first, kept in the Media Library's folder; a
//                       client from another machine sees only those in the shows folder
//
// A save or load makes its file the document's show file (DocumentServer::file).
//
// Requests run in order on a background queue (loading may hash media); replies go to the sender, errors as
// {"type": "error"}. Warnings are printed too.
// Where shows are listed: the shows folder (created when missing) and the roots a listing stays inside.
struct ShowFolders {
    std::filesystem::path shows;
    std::vector<std::filesystem::path> roots;
};

class CompositionService {
public:
    using AttachedDisplays = std::function<DisplayPlacement()>;

    // `beforeLoad` runs on the service's queue before a loaded file replaces the document.
    CompositionService(DocumentServer& document, MediaLibrary& library, AttachedDisplays displays, ShowFolders folders,
                       std::function<void()> beforeLoad = {});
    // Waits for the request in progress.
    ~CompositionService();

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);
    Message shows(const Message& message);
    std::vector<std::filesystem::path> recent() const;
    void remember(const std::filesystem::path& show);

    DocumentServer& document_;
    MediaLibrary& library_;
    AttachedDisplays displays_;
    ShowFolders folders_;
    std::function<void()> beforeLoad_;
    WorkQueue queue_;
};

} // namespace azl
