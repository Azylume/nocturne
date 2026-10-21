#include "djlink/djlink_state.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace azl::djlink {

BeatTimeline followTimeline(const OnAirBeat& beat, double engineBeat, double now)
{
    const double engineAtBeat = engineBeat - beat.beatsSince(now);
    const double place = beat.beatInBar - 1;
    const double numbered = std::round((engineAtBeat - place) / 4.0) * 4.0 + place;
    return {beat.time, numbered, beat.bpm, 0.0, 0};
}

namespace {

bool nextBeat(const Beat& previous, double previousTime, const Beat& beat, double now)
{
    const double bpm = beat.trackBpm * pitchFactor(beat.pitch);
    if (!std::isfinite(bpm) || bpm < kMinBpm || bpm > kMaxBpm)
        return false;
    const int beats = (beat.beatInBar - previous.beatInBar + 4) % 4;
    const double interval = 60.0 / bpm;
    return beats >= 1 && beats <= LinkState::kLostBeats + 1 &&
           std::abs(now - previousTime - beats * interval) <= LinkState::kBeatSlack * interval;
}

} // namespace

void LinkState::receive(const Packet& packet, double now)
{
    if (const auto* a = std::get_if<Announcement>(&packet)) {
        devices_[a->number] = {*a, now};
    } else if (const auto* b = std::get_if<Beat>(&packet)) {
        if (kindOf(b->number) != DeviceKind::Player)
            return;
        Player& p = players_[b->number];
        p.name = b->name;
        p.regularBeats = p.beat && nextBeat(*p.beat, p.beatTime, *b, now) ? p.regularBeats + 1 : 0;
        p.beat = *b;
        p.beatTime = now;
    } else if (const auto* o = std::get_if<OnAir>(&packet)) {
        for (size_t i = 0; i < o->channels.size(); ++i) {
            Player& p = players_[int(i) + 1];
            p.mixerOnAir = o->channels[i];
            p.mixerOnAirTime = now;
        }
    } else if (const auto* s = std::get_if<PlayerStatus>(&packet)) {
        if (kindOf(s->number) != DeviceKind::Player)
            return;
        Player& p = players_[s->number];
        p.name = s->name;
        p.status = *s;
        p.statusTime = now;
    } else if (const auto* m = std::get_if<MixerStatus>(&packet)) {
        mixer_ = *m;
        mixerTime_ = now;
    }
}

bool LinkState::live(const Player& player, double now) const
{
    return player.beat && now - player.beatTime < kBeatTimeout;
}

bool LinkState::grid(const Player& player, double now) const
{
    if (!live(player, now) || player.regularBeats < kGridBeats)
        return false;
    const double interval = 60.0 / bpm(player);
    if (now - player.beatTime > (kLostBeats + 1 + kBeatSlack) * interval)
        return false;
    const bool paused = player.status && now - player.statusTime < kStatusTimeout && !player.status->playing;
    return !paused;
}

std::optional<bool> LinkState::onAirFlag(const Player& player, double now) const
{
    if (player.mixerOnAir && now - player.mixerOnAirTime < kStatusTimeout)
        return player.mixerOnAir;
    if (player.status && now - player.statusTime < kStatusTimeout)
        return player.status->onAir;
    return std::nullopt;
}

bool LinkState::master(const Player& player, double now) const
{
    return player.status && now - player.statusTime < kStatusTimeout && player.status->master;
}

double LinkState::bpm(const Player& player) const
{
    return player.beat ? player.beat->trackBpm * pitchFactor(player.beat->pitch) : 0.0;
}

int LinkState::onAir(double now)
{
    std::vector<int> beating;
    std::vector<int> onAir;
    bool flagged = false;
    for (const auto& [number, player] : players_) {
        const std::optional<bool> flag = onAirFlag(player, now);
        flagged = flagged || flag.has_value();
        if (!grid(player, now))
            continue;
        beating.push_back(number);
        if (flag.value_or(false))
            onAir.push_back(number);
    }
    const std::vector<int>& pool = flagged ? onAir : beating;
    auto in = [&](int number) { return std::find(pool.begin(), pool.end(), number) != pool.end(); };
    if (pool.empty() || (!flagged && !in(followed_) && pool.size() > 1))
        followed_ = 0;
    else if (!in(followed_)) {
        followed_ = pool.front();
        for (const int number : pool)
            if (master(players_.at(number), now))
                followed_ = number;
    }
    return followed_;
}

std::optional<OnAirBeat> LinkState::onAirBeat(double now)
{
    const int player = onAir(now);
    if (!player)
        return std::nullopt;
    const Player& p = players_.at(player);
    const double tempo = bpm(p);
    if (!std::isfinite(tempo) || tempo <= 0.0)
        return std::nullopt;
    return OnAirBeat{player, p.beatTime, p.beat->beatInBar, std::clamp(tempo, kMinBpm, kMaxBpm)};
}

std::optional<MixerStatus> LinkState::mixer(double now) const
{
    if (mixer_ && now - mixerTime_ < kStatusTimeout)
        return mixer_;
    return std::nullopt;
}

void LinkState::clear()
{
    *this = {};
}

} // namespace azl::djlink
