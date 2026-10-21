#include "engine/trigger_service.hpp"

#include "document/palette.hpp"

#include <algorithm>

namespace azl {

namespace {

uint64_t idField(const Message& message, const char* key)
{
    auto it = message.find(key);
    if (it == message.end() || !it->is_number_integer() || it->get<int64_t>() < 0)
        throw ProtocolError(std::string("missing field ") + key);
    return it->get<uint64_t>();
}

std::optional<uint64_t> optionalId(const Message& message, const char* key)
{
    return message.contains(key) ? std::optional(idField(message, key)) : std::nullopt;
}

// The triggers waiting at `beat`, of one layer, of one column or all of them; each layer goes back to the cell the
// engine plays.
CancelTrigger cancelEdit(const Message& message, const Composition& composition, const TransportState& state,
                         double beat)
{
    const std::optional<uint64_t> layer = optionalId(message, "layer");
    const std::optional<uint64_t> column = optionalId(message, "column");
    CancelTrigger edit{beat, {}};
    forEachLayer(composition.layers, [&](const Layer& l) {
        const std::optional<Play>& playing = l.playing;
        if (l.group || !playing || !playing->beat || *playing->beat <= beat + kBeatEpsilon)
            return;
        if ((layer && l.id != *layer) || (column && playing->column != *column))
            return;
        auto shown = std::find_if(state.playing.begin(), state.playing.end(),
                                  [&](const TransportState::Playing& p) { return p.layer == l.id; });
        edit.layers.push_back({l.id, shown == state.playing.end() ? std::nullopt : shown->play});
    });
    if (edit.layers.empty())
        throw ProtocolError("no trigger is waiting");
    return edit;
}

// The palette after the one the show is on.
uint64_t nextPalette(const Composition& composition, const TransportState& state)
{
    if (composition.palettes.empty())
        throw ProtocolError("no palette");
    const size_t on = state.palette && state.palette->step ? *state.palette->step : activePaletteIndex(composition);
    return composition.palettes[(on + 1) % composition.palettes.size()].id;
}

} // namespace

TriggerService::TriggerService(DocumentServer& document, Engine& engine, Now now)
    : document_(document), engine_(engine), now_(std::move(now))
{
    document_.handle("trigger", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
    engine_.onClipFailure([this](const ClipKey& clip, const std::string& reason) {
        document_.broadcast(msg::clipFailed(clip.first, clip.second, reason));
    });
}

TriggerService::~TriggerService()
{
    engine_.onClipFailure(nullptr);
    document_.handle("trigger", nullptr);
}

void TriggerService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    try {
        const std::string by = message.value("by", "");
        if (!by.empty() && by != "ui" && by != "ctl")
            throw ProtocolError("a trigger is by \"ui\" or \"ctl\"");
        document_.perform(edit(message), by == "ui" ? "ui" : by == "ctl" ? "ctl" : "trigger",
                          message.value("client", ClientId(0)));
    } catch (const std::exception& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

Edit TriggerService::edit(const Message& message) const
{
    const std::string op = message.value("op", "");
    if (op == "stop")
        return StopLayer{idField(message, "layer")};
    if (op == "arm")
        return ArmClip{idField(message, "layer"), idField(message, "column"), message.value("armed", true)};
    // The beat at `now`, from the last frame's beat and tempo.
    const TransportState state = engine_.transport();
    const double beat = state.beat + (now_() - state.time) * state.tempo / 60.0;
    if (op == "deck")
        return SelectDeck{idField(message, "deck"), beat};
    if (op == "palette")
        return SelectPalette{message.value("next", false) ? nextPalette(document_.document(), state)
                                                          : idField(message, "palette"),
                             beat};
    if (op == "clip")
        return TriggerClip{idField(message, "layer"), idField(message, "column"), beat};
    if (op == "column")
        return TriggerColumn{idField(message, "column"), beat};
    if (op == "step")
        return TriggerStep{idField(message, "effect"), idField(message, "step"), beat};
    if (op == "cancel")
        return cancelEdit(message, document_.document(), state, beat);
    throw ProtocolError("unknown trigger op " + op);
}

} // namespace azl
