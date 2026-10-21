#include "venues/venue_monitor.hpp"

#include "document/composition_file.hpp"
#include "protocol/build_info.hpp"
#include "venues/gig.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace azl {

namespace {

double steadySeconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool gigOn(const Venue& venue)
{
    std::vector<GigWindow> windows;
    for (const Gig& gig : venueGigs(defaultGigsFolder(), venue.host))
        windows.push_back(gigWindow(gig));
    return inGigWindow(windows, unixTime());
}

void countSilence(const Venue& venue, VenueProbe& probe, SilentSince& silent, double now)
{
    const std::string key = venue.host + ":" + std::to_string(venue.port);
    if (!probe.health || probe.health->audioSignal != false) {
        silent.erase(key);
        return;
    }
    probe.health->silentSeconds = now - silent.try_emplace(key, now).first->second;
}

} // namespace

VenueSources VenueSources::system(TailscaleSettings settings, std::function<std::string(const Venue&)> token)
{
    auto browser = std::make_shared<Browser>();
    return {[settings] { return fetchTailscaleVenues(settings); },
            [browser] { return bonjourVenues(browser->services()); },
            [token](const Venue& venue) {
                return probeVenue(venue.host, venue.port, std::chrono::milliseconds(4000),
                                  token ? token(venue) : std::string());
            },
            gigOn,
            steadySeconds};
}

std::vector<std::string> venueLines(const VenueEntry& e)
{
    const std::string& build = e.probe.build.empty() ? "-" : e.probe.build;
    std::string head = e.venue.name + "\t" + e.venue.host + ":" + std::to_string(e.venue.port) + "\t" +
                       e.venue.source + "\t" + dotName(e.dot) + "\t" + build;
    if (!e.versionWarning.empty())
        head += "\t" + e.versionWarning;
    std::vector<std::string> lines{head};
    for (const VenueAlert& alert : e.alerts)
        lines.push_back(std::string("  ") + dotName(alert.dot) + ": " + alert.text);
    return lines;
}

VenueScan scanVenues(const VenueSources& sources, const TailscaleList& tailscale, const std::vector<Venue>& manual,
                     SilentSince* silent)
{
    const std::vector<Venue> venues =
        mergeVenues(tailscale.venues, sources.bonjour ? sources.bonjour() : std::vector<Venue>{}, manual);
    std::vector<std::future<VenueProbe>> probes;
    for (const Venue& venue : venues)
        probes.push_back(std::async(std::launch::async, [&sources, venue] {
            return venue.online ? sources.probe(venue) : VenueProbe{};
        }));
    const double now = sources.now ? sources.now() : steadySeconds();
    VenueScan scan{{}, tailscale.error};
    for (size_t i = 0; i < venues.size(); ++i) {
        VenueProbe probe = probes[i].get();
        if (silent)
            countSilence(venues[i], probe, *silent, now);
        const AlertContext context{sources.inGigWindow && sources.inGigWindow(venues[i]), buildId()};
        std::vector<VenueAlert> alerts = venueAlerts(venues[i], probe, context);
        const VenueDot dot = venueDot(alerts);
        scan.entries.push_back({venues[i], probe, std::move(alerts), dot, versionWarning(probe.build, buildId())});
    }
    return scan;
}

std::vector<StripAlert> otherVenueAlerts(const std::vector<VenueEntry>& entries, const std::string& host,
                                         uint16_t port)
{
    const auto same = [](const std::string& a, const std::string& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
            return std::tolower(x) == std::tolower(y);
        });
    };
    std::vector<StripAlert> strip;
    for (const VenueEntry& entry : entries) {
        if (entry.venue.port == port && same(entry.venue.host, host))
            continue;
        for (const VenueAlert& alert : entry.alerts)
            if (alert.kind != AlertKind::VersionMismatch)
                strip.push_back({entry.venue, alert});
    }
    return strip;
}

struct VenueMonitor::State {
    VenueSources sources;
    std::function<void()> changed;
    std::chrono::milliseconds probeEvery, listEvery;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool woken = false;
    bool relist = true;
    std::vector<Venue> manual;
    SilentSince silent;
    VenueScan scan;
    int scans = 0;
    // Held while `changed` runs, so it never runs once the monitor is gone.
    std::mutex calling;

    void run()
    {
        TailscaleList tailscale;
        auto listed = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex);
        while (!stopping) {
            const bool list = relist || std::chrono::steady_clock::now() - listed >= listEvery;
            relist = false;
            woken = false;
            const std::vector<Venue> typed = manual;
            lock.unlock();
            if (list && sources.tailscale) {
                tailscale = sources.tailscale();
                listed = std::chrono::steady_clock::now();
            }
            VenueScan next = scanVenues(sources, tailscale, typed, &silent);
            lock.lock();
            scan = std::move(next);
            ++scans;
            lock.unlock();
            {
                std::lock_guard call(calling);
                lock.lock();
                const bool gone = stopping;
                lock.unlock();
                if (changed && !gone)
                    changed();
            }
            lock.lock();
            wake.wait_for(lock, probeEvery, [&] { return stopping || woken; });
        }
    }
};

VenueMonitor::VenueMonitor(VenueSources sources, std::function<void()> changed, std::chrono::milliseconds probeEvery,
                           std::chrono::milliseconds listEvery)
    : state_(std::make_shared<State>())
{
    state_->sources = std::move(sources);
    state_->changed = std::move(changed);
    state_->probeEvery = probeEvery;
    state_->listEvery = listEvery;
    std::thread([state = state_] { state->run(); }).detach();
}

VenueMonitor::~VenueMonitor()
{
    std::lock_guard call(state_->calling);
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
    }
    state_->wake.notify_all();
}

std::vector<VenueEntry> VenueMonitor::entries() const
{
    std::lock_guard lock(state_->mutex);
    return state_->scan.entries;
}

std::string VenueMonitor::tailscaleError() const
{
    std::lock_guard lock(state_->mutex);
    return state_->scan.tailscaleError;
}

int VenueMonitor::scans() const
{
    std::lock_guard lock(state_->mutex);
    return state_->scans;
}

void VenueMonitor::addManual(const std::string& host, uint16_t port)
{
    {
        std::lock_guard lock(state_->mutex);
        for (const Venue& venue : state_->manual)
            if (venue.host == host && venue.port == port)
                return;
        state_->manual.push_back({host, host, port, "manual", true});
        state_->woken = true;
    }
    state_->wake.notify_all();
}

std::vector<Venue> VenueMonitor::manual() const
{
    std::lock_guard lock(state_->mutex);
    return state_->manual;
}

void VenueMonitor::refresh()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->relist = true;
        state_->woken = true;
    }
    state_->wake.notify_all();
}

} // namespace azl
