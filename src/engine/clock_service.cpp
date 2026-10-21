#include "engine/clock_service.hpp"

#include "engine/cue_service.hpp"

#include <cmath>
#include <optional>

namespace azl {

namespace {

Message stateMessage(const TransportState& state)
{
    Message clips = Message::array();
    for (const TransportState::Clip& clip : state.clips)
        clips.push_back({{"layer", clip.layer},
                         {"column", clip.column},
                         {"frame", clip.frame},
                         {"beats", clip.beats},
                         {"visible", clip.visible},
                         {"next", clip.next},
                         {"held", clip.held}});
    Message effects = Message::array();
    for (const EffectStepper::Shown& effect : state.effects)
        effects.push_back({{"effect", effect.effect},
                           {"step", effect.step ? Message(*effect.step) : Message(nullptr)},
                           {"next", effect.next ? Message(*effect.next) : Message(nullptr)},
                           {"held", effect.held}});
    Message message{
        {"type", "clock"},           {"op", "state"},       {"time", state.time},       {"beat", state.beat},
        {"bpm", state.bpm},          {"tempo", state.tempo}, {"nudge", state.nudge},     {"clips", clips},
        {"starting", state.starting}, {"startedAt", state.startedAt}, {"budgetLevel", state.budgetLevel},
        {"columns", {{"column", state.columns.column}, {"next", state.columns.next}, {"held", state.columns.held}}},
        {"effects", effects}};
    if (state.palette) {
        Message colours = Message::array();
        for (const Rgba& c : state.paletteColours)
            colours.push_back({c.r, c.g, c.b, c.a});
        const EffectStepper::Shown& palette = *state.palette;
        message["palette"] = {{"palette", palette.step ? Message(*palette.step) : Message(nullptr)},
                             {"next", palette.next ? Message(*palette.next) : Message(nullptr)},
                             {"held", palette.held},
                             {"colours", colours}};
    }
    return message;
}

double nudgePercent(const Message& message)
{
    auto it = message.find("percent");
    if (it == message.end() || !it->is_number())
        throw ProtocolError("missing field percent");
    const double percent = it->get<double>();
    if (!(std::abs(percent) <= ClockService::kMaxNudge))
        throw ProtocolError("nudge percent must be within -50..50");
    return percent;
}

uint64_t scrubLayer(const Message& message)
{
    auto it = message.find("layer");
    if (it == message.end() || !it->is_number_unsigned())
        throw ProtocolError("missing field layer");
    return it->get<uint64_t>();
}

double scrubPosition(const Message& message)
{
    auto it = message.find("position");
    if (it == message.end() || !it->is_number())
        throw ProtocolError("missing field position");
    const double position = it->get<double>();
    if (!(position >= 0.0 && position <= 1.0))
        throw ProtocolError("scrub position must be within 0..1");
    return position;
}

} // namespace

ClockService::ClockService(DocumentServer& document, Engine& engine, Now now)
    : document_(document), engine_(engine), now_(std::move(now))
{
    document_.handle("clock", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
    document_.watchDisconnects([this](ClientId client) { disconnected(client); });
}

ClockService::~ClockService()
{
    document_.watchDisconnects(nullptr);
    document_.handle("clock", nullptr);
}

void ClockService::disconnected(ClientId client)
{
    {
        std::lock_guard lock(nudgeMutex_);
        if (client == 0 || client != nudger_)
            return;
        nudger_ = 0;
        engine_.nudge(0.0, now_());
    }
    document_.broadcast({{"type", "clock"}, {"op", "nudge"}, {"percent", 0.0}});
}

void ClockService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    try {
        const std::string op = message.value("op", "");
        if (op == "tap")
            return tap();
        if (op == "nudge") {
            const double percent = nudgePercent(message);
            {
                std::lock_guard lock(nudgeMutex_);
                nudger_ = percent != 0.0 ? message.value("client", ClientId(0)) : 0;
                engine_.nudge(percent, now_());
            }
            return document_.broadcast({{"type", "clock"}, {"op", "nudge"}, {"percent", percent}});
        }
        if (op == "resync") {
            const double now = now_();
            rebaseTimeline(now);
            engine_.resync(now);
            return document_.broadcast({{"type", "clock"}, {"op", "resync"}});
        }
        if (op == "scrub") {
            const uint64_t layer = scrubLayer(message);
            const double position = scrubPosition(message);
            engine_.scrub(layer, position);
            return document_.broadcast({{"type", "clock"}, {"op", "scrub"}, {"layer", layer}, {"position", position}});
        }
        if (op == "state")
            return reply(stateMessage(engine_.transport()));
        throw ProtocolError("unknown clock op " + op);
    } catch (const ProtocolError& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

// The timeline moves first, so no frame sees the new beats under the old marks.
void ClockService::rebaseTimeline(double now)
{
    const Timeline timeline = document_.document().timeline;
    if (timeline != Timeline{})
        document_.perform(SetTimeline{rebasedTimeline(timeline, beatAt(engine_, now))}, "resync");
}

void ClockService::tap()
{
    std::optional<double> bpm;
    {
        std::lock_guard lock(tapMutex_);
        bpm = taps_.tap(now_());
    }
    if (bpm)
        document_.perform(SetTempo{*bpm}, "tap");
    document_.broadcast({{"type", "clock"}, {"op", "tap"}, {"bpm", bpm ? Message(*bpm) : Message(nullptr)}});
}

} // namespace azl
