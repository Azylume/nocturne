#pragma once

#include "document/wall_clock.hpp"
#include "protocol/document_server.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>

namespace azl {

// The engine's side of the venue desk (nocturne-desk, a separate process): the desk's state, kept in `file`
// (desk.json in the state folder, outside the composition: {"enabled", "port"}), and tonight's lineup from the
// composition. Served as "desk" messages to loopback clients only:
//
//   {"op": "status"} -> {"op": "status", "enabled", "port", "addresses": [LAN IPv4], "host": "<host>.local",
//                        "venue", "now" (Unix), "deck", "deckName", "tonight": null | {"date", "start", "end",
//                        "lineup": [{"slot", "dj", "at" (Unix), "time": "HH:MM", "deck", "deckName", "missing", "live",
//                        "played", "next"}], "next": null | the next slot}}
//
// "tonight" is the Gig window holding now, else the next one starting within 12 hours (lineup.hpp). Of its slots,
// "live" is the one the deck shown belongs to (slotFor), "played" one before the live slot or the last one due,
// "next" the first still to start, and "missing" one whose deck the composition no longer has.
class DeskService {
public:
    struct Options {
        std::function<double()> wall;
        UtcOffset zone = systemUtcOffset;
        // Empty: the defaults, kept in memory only.
        std::filesystem::path file;
    };

    static constexpr uint16_t kDefaultPort = 7480;

    DeskService(DocumentServer& document, Options options);
    ~DeskService();

    Message status();

private:
    void handle(const Message& message, const DocumentServer::Sink& reply);

    DocumentServer& document_;
    Options options_;
    bool enabled_ = true;
    uint16_t port_ = kDefaultPort;
};

} // namespace azl
