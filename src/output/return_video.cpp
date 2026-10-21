#include "output/return_video.hpp"

#include <algorithm>
#include <cmath>

namespace azl::return_video {

Plan plan(std::array<uint32_t, 2> composition, size_t outputs, double fps)
{
    Plan plan;
    const double aspect = composition[1] ? double(composition[0]) / double(composition[1]) : 16.0 / 9.0;
    plan.width = std::max<uint32_t>(2, uint32_t(std::lround(kHeight * aspect / 2.0)) * 2);
    plan.fps = std::clamp(fps, kMinFps, kMaxFps);
    plan.kbps = std::min<uint32_t>(kMaxOutputKbps, kBudgetKbps / uint32_t(std::max<size_t>(outputs, 1)));
    return plan;
}

std::vector<std::vector<uint8_t>> nalUnits(const std::vector<uint8_t>& avcc)
{
    std::vector<std::vector<uint8_t>> units;
    size_t at = 0;
    while (at + 4 <= avcc.size()) {
        const size_t length = size_t(avcc[at]) << 24 | size_t(avcc[at + 1]) << 16 | size_t(avcc[at + 2]) << 8 |
                              size_t(avcc[at + 3]);
        at += 4;
        if (length > avcc.size() - at)
            return {};
        units.emplace_back(avcc.begin() + long(at), avcc.begin() + long(at + length));
        at += length;
    }
    if (at != avcc.size())
        return {};
    return units;
}

std::vector<uint8_t> annexB(const Packet& packet)
{
    static constexpr uint8_t kStart[] = {0, 0, 0, 1};
    std::vector<uint8_t> stream;
    for (const auto* avcc : {&packet.config, &packet.data})
        for (const auto& unit : nalUnits(*avcc)) {
            stream.insert(stream.end(), std::begin(kStart), std::end(kStart));
            stream.insert(stream.end(), unit.begin(), unit.end());
        }
    return stream;
}

Message frameMessage(const std::string& display, uint64_t screen, uint64_t frame, const Plan& plan,
                     const Packet& packet)
{
    Message message = {{"type", "video"},     {"op", "frame"},          {"display", display},
                       {"screen", screen},    {"frame", frame},         {"key", packet.key},
                       {"width", plan.width}, {"height", plan.height}, {kReturnPayload, Message::binary(packet.data)}};
    if (!packet.config.empty())
        message["config"] = Message::binary(packet.config);
    return message;
}

std::optional<Packet> packetOf(const Message& message)
{
    const auto data = message.find(kReturnPayload);
    if (message.value("type", "") != "video" || message.value("op", "") != "frame" || data == message.end() ||
        !data->is_binary())
        return std::nullopt;
    Packet packet{.data = data->get_binary(), .key = message.value("key", false)};
    if (const auto config = message.find("config"); config != message.end() && config->is_binary())
        packet.config = config->get_binary();
    return packet;
}

bool Gate::pass(const std::string& output, bool key, bool congested)
{
    std::lock_guard lock(mutex_);
    if (congested) {
        broken_.insert(output);
        return false;
    }
    if (key)
        broken_.erase(output);
    return !broken_.contains(output);
}

} // namespace azl::return_video
