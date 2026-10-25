#pragma once

#include "document/composition.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <set>
#include <string>
#include <utility>

namespace azl {

Edit apply(Composition& composition, AddMacro& edit);
Edit apply(Composition& composition, RemoveMacro& edit);
Edit apply(Composition& composition, SetMacroValue& edit);
Edit apply(Composition& composition, AddMacroTarget& edit);
Edit apply(Composition& composition, RemoveMacroTarget& edit);
Edit apply(Composition& composition, SetMacroTarget& edit);
Edit apply(Composition& composition, AddModulator& edit);
Edit apply(Composition& composition, RemoveModulator& edit);
Edit apply(Composition& composition, SetModulator& edit);
Edit apply(Composition& composition, SetMacros& edit);

nlohmann::json toJson(const AddMacro& e);
nlohmann::json toJson(const RemoveMacro& e);
nlohmann::json toJson(const SetMacroValue& e);
nlohmann::json toJson(const AddMacroTarget& e);
nlohmann::json toJson(const RemoveMacroTarget& e);
nlohmann::json toJson(const SetMacroTarget& e);
nlohmann::json toJson(const AddModulator& e);
nlohmann::json toJson(const RemoveModulator& e);
nlohmann::json toJson(const SetModulator& e);
nlohmann::json toJson(const SetMacros& e);

std::optional<Edit> modulationEditFromJson(const std::string& op, const nlohmann::json& j);

// The range, in the parameter's own units, an audio modulator made with Animate sweeps on a commonly animated
// parameter, so a new one looks right on any music: Reveal's progress 0.35..0.65, Stripper's width 0.3..0.7, Colorize's
// brightness 0.5..1, Transform's scale 0.9..1.15 and a layer's opacity 0.4..1. None for other parameters.
std::optional<std::pair<double, double>> animationRange(const Composition& composition, const ParamRef& param);

// Drops the macro targets and modulators on these layers or effects, naming each in `dropped`; returns the edit
// restoring them, or nothing when none was there.
std::optional<SetMacros> dropTargets(Composition& composition, const std::set<uint64_t>& layers,
                                     const std::set<uint64_t>& effects, std::vector<std::string>& dropped);

// Puts every audio modulator on the preset, everything else as it is, in one undoable edit.
SetMacros setEnvelopes(const Composition& composition, EnvelopePreset preset);

struct ParamDef;

const Layer* layerById(const Composition& composition, uint64_t id);

// The parameter's range, or nothing when it is not in the composition.
std::optional<std::pair<double, double>> paramRange(const Composition& composition, const ParamRef& param);
// The effect parameter's definition; null for a layer's own parameters and when it is not in the composition.
const ParamDef* paramDef(const Composition& composition, const ParamRef& param);

void to_json(nlohmann::json& j, const Curve& curve);
void from_json(const nlohmann::json& j, Curve& curve);
void to_json(nlohmann::json& j, const LfoPoint& point);
void from_json(const nlohmann::json& j, LfoPoint& point);
void to_json(nlohmann::json& j, const ParamRef& param);
void from_json(const nlohmann::json& j, ParamRef& param);
void to_json(nlohmann::json& j, const MacroTarget& target);
void from_json(const nlohmann::json& j, MacroTarget& target);
void to_json(nlohmann::json& j, const Macro& macro);
void from_json(const nlohmann::json& j, Macro& macro);
void to_json(nlohmann::json& j, const Modulator& modulator);
void from_json(const nlohmann::json& j, Modulator& modulator);

} // namespace azl
