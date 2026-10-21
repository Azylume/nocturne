#include "ui/client/return_video_client.hpp"

#include <set>

namespace azl::ui {

bool ReturnVideoClient::receive(const Message& message)
{
    const std::string op = message.value("op", "");
    std::lock_guard lock(mutex_);
    if (op == "watch") {
        std::set<std::string> shown;
        for (const Message& output : message.value("outputs", Message::array()))
            shown.insert(output.value("display", ""));
        std::erase_if(outputs_, [&](const auto& output) { return !shown.contains(output.first); });
        return true;
    }
    const std::optional<return_video::Packet> packet = return_video::packetOf(message);
    if (!packet)
        return false;
    bytes_ += packet->data.size() + packet->config.size();
    Output& output = outputs_[message.value("display", "")];
    std::optional<H264Decoder::Picture> picture = output.decoder->decode(*packet);
    if (!picture)
        return false;
    output.tile = VideoTile{message.value("display", ""), message.value("screen", uint64_t(0)),
                            message.value("frame", uint64_t(0)), std::move(*picture)};
    return true;
}

std::optional<VideoTile> ReturnVideoClient::tile(const std::string& display) const
{
    std::lock_guard lock(mutex_);
    const auto it = outputs_.find(display);
    return it == outputs_.end() ? std::nullopt : it->second.tile;
}

uint64_t ReturnVideoClient::bytes() const
{
    std::lock_guard lock(mutex_);
    return bytes_;
}

void ReturnVideoClient::clear()
{
    std::lock_guard lock(mutex_);
    outputs_.clear();
}

} // namespace azl::ui
