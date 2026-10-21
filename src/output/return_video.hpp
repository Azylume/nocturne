#pragma once

#include "protocol/messages.hpp"
#include "protocol/return_stream.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace azl {

// The return video's budget: each output of a venue machine streamed small, as H.264, for a Director watching from
// home over a 4G uplink. Video of all outputs stays inside kBudgetKbps, which leaves room in the venue's 1-2 Mbit/s
// for the return audio.
namespace return_video {

inline constexpr uint32_t kHeight = 240;
inline constexpr double kFps = 10.0;
inline constexpr double kMinFps = 1.0;
inline constexpr double kMaxFps = 15.0;
// All outputs together; each output gets an equal share, at most kMaxOutputKbps.
inline constexpr uint32_t kBudgetKbps = 1000;
inline constexpr uint32_t kMaxOutputKbps = 400;
// A key frame at least this often, so a viewer that missed frames shows pictures again soon.
inline constexpr double kKeySeconds = 2.0;

struct Plan {
    uint32_t width = 0;
    uint32_t height = kHeight;
    double fps = kFps;
    // Per output.
    uint32_t kbps = 0;
};

// The picture keeps the composition's aspect at kHeight lines, with an even width. `fps` is clamped to
// kMinFps..kMaxFps.
Plan plan(std::array<uint32_t, 2> composition, size_t outputs, double fps = kFps);

// An H.264 access unit as it travels: its NAL units each behind a 4-byte big-endian length (AVCC), and on a key frame
// the parameter sets (SPS, then PPS) in the same layout.
struct Packet {
    std::vector<uint8_t> config;
    std::vector<uint8_t> data;
    bool key = false;
};

// The NAL units of an AVCC buffer; empty when a length runs past its end.
std::vector<std::vector<uint8_t>> nalUnits(const std::vector<uint8_t>& avcc);
// The packet as an Annex B byte stream (start codes), parameter sets first, as a raw .h264 file holds it.
std::vector<uint8_t> annexB(const Packet& packet);

// The protocol, "video" messages; machine-local, outside the document:
//   {"type": "video", "op": "watch", "on": true | false, "fps"?: 10}
//       starts or ends this client's watch; the reply, sent again whenever the outputs change while it watches:
//       {"type": "video", "op": "watch", "on", "fps", "width", "height", "kbps" (per output), "budgetKbps",
//        "outputs": [{"display", "screen"}]}
//   {"type": "video", "op": "state"}
//       -> {"type": "video", "op": "state", "watchers", "outputs", "pictures", "bytes", "hardware": true | false | null}
// While a client watches, each output's newest picture comes as
//   {"type": "video", "op": "frame", "display", "screen", "frame", "key", "width", "height",
//    "payload": <binary access unit>, "config"?: <binary parameter sets, on key frames>}
// A watch starts at a key frame. Frames are return stream packets (protocol/return_stream.hpp): past kMaxPendingReturn
// unsent messages a viewer's link drops them, and through Gate that output's later frames until its next key frame.
Message frameMessage(const std::string& display, uint64_t screen, uint64_t frame, const Plan& plan,
                     const Packet& packet);
// Null when the message is no frame with a payload.
std::optional<Packet> packetOf(const Message& message);

// Decides per viewer which frames a slow link sends: once a frame of an output is dropped (the link is `congested`),
// that output's later frames are dropped too until its next key frame, which the decoder needs to go on.
// Thread-safe.
class Gate {
public:
    bool pass(const std::string& output, bool key, bool congested);

private:
    std::mutex mutex_;
    std::set<std::string> broken_;
};

} // namespace return_video

} // namespace azl
