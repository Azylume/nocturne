#include "protocol/document_mirror.hpp"

namespace azl {

std::optional<Message> DocumentMirror::receive(const Message& message)
{
    const std::string type = message.value("type", "");
    if (type == "snapshot") {
        composition_ = message.at("composition").get<Composition>();
        change_ = message.at("change").get<uint64_t>();
        synced_ = true;
        return std::nullopt;
    }
    if (type != "change" || !synced_)
        return std::nullopt;

    const uint64_t number = message.at("change").get<uint64_t>();
    if (number <= change_)
        return std::nullopt;
    if (number != change_ + 1) {
        synced_ = false;
        return msg::snapshotRequest();
    }
    try {
        Edit edit = editFromJson(message);
        applyEdit(composition_, edit);
    } catch (...) {
        synced_ = false;
        throw;
    }
    change_ = number;
    return std::nullopt;
}

} // namespace azl
