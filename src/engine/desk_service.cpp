#include "engine/desk_service.hpp"

#include "document/composition_file.hpp"
#include "document/lineup.hpp"
#include "net/lan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace azl {

namespace {

const Deck* findDeck(const Composition& composition, uint64_t id)
{
    for (const Deck& deck : composition.decks)
        if (deck.id == id)
            return &deck;
    return nullptr;
}

std::string clockAt(int64_t unix, const UtcOffset& zone)
{
    return clockText(localMinute(unix, zone));
}

Message slotJson(const Composition& composition, const LineupSlot& slot, const UtcOffset& zone)
{
    const Deck* deck = findDeck(composition, slot.deck);
    return {{"slot", slot.slot},
            {"dj", slot.dj},
            {"at", slot.start},
            {"time", clockAt(slot.start, zone)},
            {"deck", slot.deck},
            {"deckName", deck ? deck->name : ""},
            {"missing", !deck}};
}

} // namespace

DeskService::DeskService(DocumentServer& document, Options options)
    : document_(document), options_(std::move(options))
{
    if (const std::optional<Message> saved = options_.file.empty() ? std::nullopt : readJsonFile(options_.file)) {
        enabled_ = saved->value("enabled", enabled_);
        port_ = saved->value("port", port_);
    } else if (!options_.file.empty()) {
        try {
            writeFileAtomically(options_.file, Message{{"enabled", enabled_}, {"port", port_}}.dump(2) + "\n", false);
        } catch (const std::exception& e) {
            std::printf("desk: %s\n", e.what());
        }
    }
    document_.handle("desk", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

DeskService::~DeskService()
{
    document_.handle("desk", nullptr);
}

Message DeskService::status()
{
    const Composition composition = document_.document();
    const double now = options_.wall();
    const Deck* live = findDeck(composition, composition.deck);
    const std::string host = bonjourName();
    Message status{{"type", "desk"},
                   {"op", "status"},
                   {"enabled", enabled_},
                   {"port", port_},
                   {"addresses", lanAddresses()},
                   {"host", host},
                   {"venue", host.substr(0, host.size() - 6)},
                   {"now", now},
                   {"deck", composition.deck},
                   {"deckName", live ? live->name : ""},
                   {"tonight", nullptr}};
    const std::optional<Night> night = tonight(composition, now, options_.zone);
    if (!night)
        return status;
    const LineupSlot* shown = slotFor(*night, composition.deck, now);
    // Slots before this one have played: the live one, or the last whose time has come, whichever is later.
    size_t current = shown ? shown->slot - 1 : 0;
    for (size_t i = 0; i < night->lineup.size(); ++i)
        if (double(night->lineup[i].start) <= now)
            current = std::max(current, i);
    Message lineup = Message::array();
    Message next = nullptr;
    for (size_t i = 0; i < night->lineup.size(); ++i) {
        const LineupSlot& slot = night->lineup[i];
        Message entry = slotJson(composition, slot, options_.zone);
        entry["live"] = &slot == shown;
        entry["played"] = i < current;
        entry["next"] = next.is_null() && double(slot.start) > now;
        if (entry["next"] == true)
            next = entry;
        lineup.push_back(std::move(entry));
    }
    status["tonight"] = {{"date", night->window.date},
                         {"start", isoTime(double(night->start), options_.zone(night->start))},
                         {"end", isoTime(double(night->end), options_.zone(night->end))},
                         {"lineup", lineup},
                         {"next", next}};
    return status;
}

void DeskService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    if (!message.value("loopback", false))
        return reply({{"type", "error"}, {"message", "the desk protocol answers only on this machine"}});
    if (message.value("op", "") != "status")
        return reply({{"type", "error"}, {"message", "unknown desk op"}});
    reply(status());
}

} // namespace azl
