#include "media/library_service.hpp"

#include "media/stills.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

#include <memory>
#include <stdexcept>

namespace azl {

namespace {

struct Request {
    LibraryService* service;
    Message message;
    DocumentServer::Sink reply;
};

// Stills are kept next to the library index, one file per hash and width.
std::vector<uint8_t> still(MediaLibrary& library, const std::string& hash, uint32_t width, StillCounts& counts)
{
    const std::filesystem::path cached = library.root() / "stills" / (hash + "-" + std::to_string(width) + ".jpg");
    if (std::ifstream in{cached, std::ios::binary})
        return {std::istreambuf_iterator<char>(in), {}};
    const std::optional<MediaEntry> entry = library.find(hash);
    if (!entry || entry->status != MediaStatus::Ok)
        throw std::runtime_error(entry ? entry->reason : "not in the media library");
    std::vector<uint8_t> jpeg = videoStill(entry->path, width);
    ++counts.rendered;
    std::error_code ec;
    std::filesystem::create_directories(cached.parent_path(), ec);
    std::ofstream(cached, std::ios::binary).write(reinterpret_cast<const char*>(jpeg.data()), std::streamsize(jpeg.size()));
    return jpeg;
}

Message stillReply(MediaLibrary& library, const Message& message, StillCounts& counts)
{
    const std::string op = message.value("op", "");
    if (op == "still")
        ++counts.asked;
    const std::string hash = message.value("hash", "");
    const uint32_t width = op == "still" ? kStillWidth : std::clamp(message.value("width", 160u), 16u, 1024u);
    Message reply = {{"type", "library"}, {"op", op}, {"hash", hash}};
    if (op == "thumbnail")
        reply["width"] = width;
    try {
        reply["jpeg"] = Message::binary(still(library, hash, width, counts));
    } catch (const std::exception& e) {
        reply["error"] = e.what();
    }
    return reply;
}

Message relink(MediaLibrary& library, const Message& message)
{
    const std::string hash = message.value("hash", "");
    const std::string path = message.value("path", "");
    if (!path.empty() && !std::filesystem::path(path).is_absolute())
        throw std::runtime_error("relink needs an absolute path on the engine's machine");
    std::optional<MediaEntry> entry = path.empty() ? library.find(hash) : std::optional(library.add(path));
    if (!entry)
        throw std::runtime_error("media " + hash + " is not in the media library");
    if (entry->status != MediaStatus::Ok)
        throw std::runtime_error(entry->name + ": " + (entry->reason.empty() ? statusName(entry->status) : entry->reason));
    return {{"type", "library"}, {"op", "relink"}, {"hash", hash}, {"entry", *entry}};
}

Message answer(MediaLibrary& library, const Message& message, StillCounts& counts)
{
    const std::string op = message.value("op", "");
    if (op == "relink")
        return relink(library, message);
    if (op == "thumbnail" || op == "still")
        return stillReply(library, message, counts);
    if (op == "list")
        return {{"type", "library"}, {"op", "list"}, {"entries", library.entries()}};
    if (op != "add")
        throw std::runtime_error("unknown library op " + op);
    const std::string path = message.value("path", "");
    if (!std::filesystem::path(path).is_absolute())
        throw std::runtime_error("library add needs an absolute path on the engine's machine");
    return {{"type", "library"}, {"op", "add"}, {"entry", library.add(path)}};
}

} // namespace

LibraryService::LibraryService(DocumentServer& document, MediaLibrary& library,
                               std::function<void(const std::string&)> found)
    : document_(document), library_(library), found_(std::move(found)), queue_(dispatch_queue_create("nocturne.library", DISPATCH_QUEUE_SERIAL))
{
    document_.handle("library", [this](const Message& message, DocumentServer::Sink reply) {
        dispatch_async_f(queue_, new Request{this, message, std::move(reply)}, [](void* context) {
            std::unique_ptr<Request> request(static_cast<Request*>(context));
            request->service->handle(request->message, request->reply);
        });
    });
}

LibraryService::~LibraryService()
{
    document_.handle("library", nullptr);
    dispatch_sync_f(queue_, nullptr, [](void*) {});
    dispatch_release(queue_);
}

void LibraryService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    try {
        const Message answered = answer(library_, message, stills_);
        if (found_ && answered.contains("entry") && answered["entry"].value("status", "") == "ok")
            found_(answered["entry"].value("hash", ""));
        reply(answered);
    } catch (const std::exception& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

} // namespace azl
