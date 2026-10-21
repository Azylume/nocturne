#pragma once

#include "venues/venues.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace azl {

// Where the venue list and each venue's state come from; the system's by default, fakes in tests.
struct VenueSources {
    std::function<TailscaleList()> tailscale;
    std::function<std::vector<Venue>()> bonjour;
    std::function<VenueProbe(const Venue&)> probe;
    // Whether one of the venue's Gigs is on now; unset, none is.
    std::function<bool(const Venue&)> inGigWindow;
    // Seconds on a steady clock; unset, the system's.
    std::function<double()> now;

    // The Tailscale API with these settings, a Bonjour browser, probeVenue with the pairing token `token` names for
    // each venue, and the Director's Gigs.
    static VenueSources system(TailscaleSettings settings,
                               std::function<std::string(const Venue&)> token = {});
};

// One venue in the menu.
struct VenueEntry {
    Venue venue;
    VenueProbe probe;
    std::vector<VenueAlert> alerts;
    VenueDot dot = VenueDot::Offline;
    // Empty while the venue's build matches this binary's, or is unknown.
    std::string versionWarning;
};

// The venue's line for `nocturne-ctl venues` (name, address, source, dot, build, version warning), then one indented
// line per alert: its dot and the text the UI shows.
std::vector<std::string> venueLines(const VenueEntry& entry);

struct StripAlert {
    Venue venue;
    VenueAlert alert;
};

// The alerts of every venue but the one at host:port (the one in view), for the alert strip; a build mismatch stays in
// the menu.
std::vector<StripAlert> otherVenueAlerts(const std::vector<VenueEntry>& entries, const std::string& host,
                                         uint16_t port);

struct VenueScan {
    std::vector<VenueEntry> entries;
    std::string tailscaleError;
};

// Since when each venue ("host:port") has had no audio signal, across scans.
using SilentSince = std::map<std::string, double>;

// Lists the venues from `tailscale` (fetched by the caller), Bonjour and the typed ones, and probes them all at once.
// With `silent`, a venue's silence counts from the first scan that found it silent.
VenueScan scanVenues(const VenueSources& sources, const TailscaleList& tailscale, const std::vector<Venue>& manual,
                     SilentSince* silent = nullptr);

// Keeps the venue list and every venue's dot current on its own thread: the Tailscale list now and then, Bonjour and
// a probe of each venue every few seconds, which fetches its health and nothing else. Never touches the UI's
// connection. Destroying it does not wait for a probe under way; `changed` is never called after.
class VenueMonitor {
public:
    // `changed` is called on the monitor's thread after each scan.
    VenueMonitor(VenueSources sources, std::function<void()> changed = {},
                 std::chrono::milliseconds probeEvery = std::chrono::seconds(5),
                 std::chrono::milliseconds listEvery = std::chrono::seconds(60));
    ~VenueMonitor();

    std::vector<VenueEntry> entries() const;
    std::string tailscaleError() const;
    // Scans have finished since the start.
    int scans() const;
    // A typed host joins the list (once) and is probed from the next scan, which starts now.
    void addManual(const std::string& host, uint16_t port = kDefaultPort);
    std::vector<Venue> manual() const;
    // Scans now, the Tailscale list too.
    void refresh();

    struct State;

private:
    std::shared_ptr<State> state_;
};

} // namespace azl
