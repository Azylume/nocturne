#include "engine/cue_service.hpp"

#include <cmath>

namespace azl {

namespace {

constexpr double kBar = 4.0;
constexpr const char* kModeNames[] = {"off", "next", "random"};

Message beatOrNull(double beat)
{
    return std::isfinite(beat) ? Message(beat) : Message(nullptr);
}

Message firingsJson(const std::vector<CueFiring>& firings)
{
    Message list = Message::array();
    for (const CueFiring& f : firings)
        list.push_back({{"cue", f.cue}, {"beat", f.beat}, {"until", beatOrNull(f.until)}});
    return list;
}

// Each cued layer's Autopilot as it plays, the document's values under the Cues'.
Message autopilotJson(const Composition& composition, const std::vector<CueAutopilot>& cued)
{
    Message list = Message::array();
    forEachLayer(composition.layers, [&](const Layer& layer) {
        for (const CueAutopilot& a : cued)
            if (a.layer == layer.id) {
                const Autopilot autopilot =
                    cuedAutopilot(layer, layer.playing ? columnDeck(composition, layer.playing->column) : 0, cued);
                list.push_back(
                    {{"layer", layer.id}, {"mode", kModeNames[size_t(autopilot.mode)]}, {"bars", autopilot.bars}});
            }
    });
    return list;
}

Message stateMessage(const Composition& composition, double beat)
{
    CueState state;
    cueStateAt(composition, beat, state);
    Message macros = Message::array();
    for (const CueMacro& m : state.macros)
        macros.push_back({{"macro", m.macro}, {"value", m.value}});
    return {{"type", "cue"},
            {"op", "state"},
            {"beat", beat},
            {"section", state.section ? Message(sectionName(*state.section)) : Message(nullptr)},
            {"start", beatOrNull(state.start)},
            {"end", beatOrNull(state.end)},
            {"sections", Message(composition.timeline)["sections"]},
            {"active", firingsJson(state.active)},
            {"upcoming", firingsJson(upcomingCues(composition, beat))},
            {"autopilot", autopilotJson(composition, state.autopilot)},
            {"macros", macros}};
}

Timeline mark(const Composition& composition, const Message& message, double now)
{
    auto name = message.find("section");
    if (name == message.end() || !name->is_string())
        throw ProtocolError("missing field section");
    const Section section = sectionFromName(name->get<std::string>());
    auto bar = message.find("bar");
    if (bar == message.end())
        return markSection(composition.timeline, section, std::ceil(now / kBar - kBeatEpsilon) * kBar, now);
    if (!bar->is_number_integer() || bar->get<int64_t>() < 1 || bar->get<int64_t>() > kMaxCueBar)
        throw ProtocolError("bar must be within 1.." + std::to_string(kMaxCueBar));
    const double at = double(bar->get<int64_t>() - 1) * kBar;
    if (at < now - kBeatEpsilon)
        throw ProtocolError("bar " + std::to_string(bar->get<int64_t>()) + " has passed");
    return markSection(composition.timeline, section, at, now);
}

Timeline skipNext(const Composition& composition, uint64_t cue, double now)
{
    for (const CueFiring& firing : upcomingCues(composition, now)) {
        if (firing.cue != cue)
            continue;
        Timeline timeline = composition.timeline;
        timeline.skips.push_back({cue, firing.beat});
        return timeline;
    }
    throw ProtocolError("cue " + std::to_string(cue) + " is not scheduled");
}

Timeline cancel(const Composition& composition, const Message& message, double now)
{
    if (auto cue = message.find("cue"); cue != message.end()) {
        if (!cue->is_number_unsigned())
            throw ProtocolError("bad field cue");
        return skipNext(composition, cue->get<uint64_t>(), now);
    }
    std::optional<double> at;
    if (auto bar = message.find("bar"); bar != message.end()) {
        if (!bar->is_number_integer())
            throw ProtocolError("bad field bar");
        at = double(bar->get<int64_t>() - 1) * kBar;
    }
    Timeline timeline = composition.timeline;
    const auto cancelled = [&](const SectionMark& m) {
        return m.beat > now + kBeatEpsilon && (!at || std::abs(m.beat - *at) < kBeatEpsilon);
    };
    if (!std::erase_if(timeline.sections, cancelled))
        throw ProtocolError(at ? "no section is scheduled on that bar" : "no section is scheduled");
    return timeline;
}

} // namespace

double beatAt(const Engine& engine, double now)
{
    const TransportState state = engine.transport();
    return state.beat + (now - state.time) * state.tempo / 60.0;
}

CueService::CueService(DocumentServer& document, Engine& engine, Now now)
    : document_(document), engine_(engine), now_(std::move(now))
{
    document_.handle("cue", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

CueService::~CueService()
{
    document_.handle("cue", nullptr);
}

void CueService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    try {
        const std::string op = message.value("op", "");
        const Composition composition = document_.document();
        const double now = beatAt(engine_, now_());
        const ClientId client = message.value("client", ClientId(0));
        if (op == "state")
            return reply(stateMessage(composition, now));
        if (op == "mark")
            return document_.perform(SetTimeline{mark(composition, message, now)}, "cue", client);
        if (op == "cancel")
            return document_.perform(SetTimeline{cancel(composition, message, now)}, "cue", client);
        if (op == "clear")
            return document_.perform(SetTimeline{}, "cue", client);
        throw ProtocolError("unknown cue op " + op);
    } catch (const std::exception& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

} // namespace azl
