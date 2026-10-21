#pragma once

#include "document/composition.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

// What the JSON units (edit_json.cpp, composition_json.cpp) share: the names of the enums and reading fields.

namespace azl::json_fields {

inline constexpr std::array<std::pair<SyncMode, const char*>, 2> kSyncNames{{
    {SyncMode::Timeline, "timeline"},
    {SyncMode::Bpm, "bpm"},
}};

inline constexpr std::array<std::pair<PlayMode, const char*>, 6> kPlayModeNames{{
    {PlayMode::Loop, "loop"},
    {PlayMode::Bounce, "bounce"},
    {PlayMode::OnceClear, "onceClear"},
    {PlayMode::OnceHold, "onceHold"},
    {PlayMode::Reverse, "reverse"},
    {PlayMode::RandomJump, "randomJump"},
}};

inline constexpr std::array<std::pair<AutopilotMode, const char*>, 3> kAutopilotNames{{
    {AutopilotMode::Off, "off"},
    {AutopilotMode::Next, "next"},
    {AutopilotMode::Random, "random"},
}};

inline constexpr std::array<std::pair<TransitionMode, const char*>, 7> kTransitionNames{{
    {TransitionMode::Alpha, "alpha"},
    {TransitionMode::Wipe, "wipe"},
    {TransitionMode::Dissolve, "dissolve"},
    {TransitionMode::Push, "push"},
    {TransitionMode::WindowSlice, "windowSlice"},
    {TransitionMode::Radial, "radial"},
    {TransitionMode::RandomSquares, "randomSquares"},
}};

template <typename E, size_t N>
const char* nameOf(const std::array<std::pair<E, const char*>, N>& names, E value)
{
    for (const auto& [v, name] : names)
        if (v == value)
            return name;
    return "";
}

template <typename E, size_t N>
E valueOf(const std::array<std::pair<E, const char*>, N>& names, const std::string& name, const char* what)
{
    for (const auto& [v, n] : names)
        if (name == n)
            return v;
    throw ProtocolError(std::string("unknown ") + what + " " + name);
}

template <typename T>
T field(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end())
        throw ProtocolError(std::string("missing field ") + key);
    try {
        return it->get<T>();
    } catch (const nlohmann::json::exception&) {
        throw ProtocolError(std::string("bad field ") + key);
    }
}

template <typename T>
std::optional<T> optionalField(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key))
        return std::nullopt;
    return field<T>(j, key);
}

inline std::optional<SyncMode> syncField(const nlohmann::json& j)
{
    if (auto name = optionalField<std::string>(j, "sync"))
        return valueOf(kSyncNames, *name, "sync mode");
    return std::nullopt;
}

inline std::optional<PlayMode> playModeField(const nlohmann::json& j)
{
    if (auto name = optionalField<std::string>(j, "playMode"))
        return valueOf(kPlayModeNames, *name, "play mode");
    return std::nullopt;
}

inline std::optional<AutopilotMode> autopilotModeField(const nlohmann::json& j)
{
    if (auto name = optionalField<std::string>(j, "mode"))
        return valueOf(kAutopilotNames, *name, "autopilot mode");
    return std::nullopt;
}

inline std::optional<TransitionMode> transitionModeField(const nlohmann::json& j)
{
    if (auto name = optionalField<std::string>(j, "mode"))
        return transitionFromName(*name);
    return std::nullopt;
}

// Null and absent are both none.
inline std::optional<double> nullableBeats(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key) || j[key].is_null())
        return std::nullopt;
    return field<double>(j, key);
}

// Null and absent are both none.
inline std::optional<std::array<double, 2>> positionField(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key) || j[key].is_null())
        return std::nullopt;
    return field<std::array<double, 2>>(j, key);
}

inline nlohmann::json beatsOrNull(const std::optional<double>& beats)
{
    return beats ? nlohmann::json(*beats) : nlohmann::json(nullptr);
}

inline Transport transportFromJson(const nlohmann::json& j)
{
    Transport t;
    if (!j.is_object())
        return t;
    t.sync = syncField(j).value_or(t.sync);
    t.playMode = playModeField(j).value_or(t.playMode);
    t.beats = optionalField<int>(j, "beats").value_or(t.beats);
    t.jumpInterval = optionalField<double>(j, "jumpInterval").value_or(t.jumpInterval);
    t.jumpDistance = optionalField<double>(j, "jumpDistance").value_or(t.jumpDistance);
    t.speed = optionalField<double>(j, "speed").value_or(t.speed);
    t.in = optionalField<double>(j, "in").value_or(t.in);
    t.out = optionalField<double>(j, "out").value_or(t.out);
    return t;
}

} // namespace azl::json_fields

namespace azl {

// A layer's settings by deck id, as composition files and addDeck hold them; `what` names the key in errors.
nlohmann::json deckLayersJson(const std::map<uint64_t, DeckLayer>& settings);
std::map<uint64_t, DeckLayer> deckLayersFromJson(const nlohmann::json& j, const char* what);

} // namespace azl
