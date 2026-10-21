#include "engine/wall_cue_service.hpp"

#include "engine/autopilot_stepper.hpp"
#include "engine/cue_service.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace azl {

namespace {

constexpr double kBar = 4.0;

} // namespace

WallCueService::WallCueService(DocumentServer& document, Engine& engine, Now now, Wall wall, UtcOffset zone,
                               std::optional<double> period)
    : document_(document), engine_(engine), now_(std::move(now)), wall_(std::move(wall)), zone_(std::move(zone))
{
    if (!period)
        return;
    thread_ = std::thread([this, seconds = *period] {
        std::unique_lock lock(mutex_);
        while (!stop_) {
            wake_.wait_for(lock, std::chrono::duration<double>(seconds));
            if (stop_)
                break;
            lock.unlock();
            poll();
            lock.lock();
        }
    });
}

WallCueService::~WallCueService()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void WallCueService::restart()
{
    std::lock_guard lock(mutex_);
    last_.reset();
}

void WallCueService::refresh()
{
    const uint64_t change = document_.change();
    if (change_ == change && composition_)
        return;
    change_ = change;
    composition_ = std::make_shared<const Composition>(document_.document());
}

void WallCueService::poll()
{
    std::lock_guard lock(mutex_);
    const double wall = wall_();
    const bool starting = !last_;
    const double from = starting ? wall - kCatchUp : *last_;
    last_ = wall;
    if (wall <= from)
        return;
    refresh();
    std::vector<std::pair<int64_t, const Cue*>> due;
    for (const Cue& cue : composition_->cues) {
        if (!cue.at || !cue.enabled)
            continue;
        const std::optional<int64_t> at = nextAt(*cue.at, int64_t(std::floor(from)), zone_);
        if (!at || double(*at) > wall)
            continue;
        due.emplace_back(*at, &cue);
    }
    std::stable_sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::shared_ptr<const Composition> composition = composition_;
    uint64_t shown = composition->deck;
    for (const auto& entry : due) {
        const Cue& cue = *entry.second;
        if (cue.deck && *cue.deck == shown) {
            if (!starting)
                skip(cue);
            continue;
        }
        fire(*composition, cue);
        if (cue.deck)
            shown = *cue.deck;
    }
}

void WallCueService::skip(const Cue& cue)
{
    const std::string why = "deck " + std::to_string(*cue.deck) + " already live";
    std::printf("cue %s: %s\n", cue.name.c_str(), why.c_str());
    document_.broadcast({{"type", "cue"}, {"op", "skipped"}, {"cue", cue.id}, {"name", cue.name}, {"deck", *cue.deck},
                         {"reason", why}});
}

void WallCueService::fire(const Composition& composition, const Cue& cue)
{
    try {
        Composition shown = composition;
        if (cue.deck) {
            document_.perform(SelectDeck{*cue.deck}, "cue");
            shown.deck = *cue.deck;
        }
        const uint64_t column = cue.column ? cueColumn(shown, *cue.column) : 0;
        if (!column)
            return;
        const double bar = std::ceil(beatAt(engine_, now_()) / kBar - kBeatEpsilon) * kBar;
        document_.perform(TriggerColumn{column, bar}, "cue");
    } catch (const std::exception& e) {
        std::printf("cue: %s at %s did not fire: %s\n", cue.name.c_str(), clockText(cue.at->minute).c_str(), e.what());
    }
}

} // namespace azl
