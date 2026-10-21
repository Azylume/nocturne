#pragma once

#include "document/composition.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace azl {

Edit apply(Composition& composition, SetSliceWarp& edit);
Edit apply(Composition& composition, SetSoftEdges& edit);
Edit apply(Composition& composition, SetSliceColor& edit);
Edit apply(Composition& composition, SetScreenColor& edit);
Edit apply(Composition& composition, AddMask& edit);
Edit apply(Composition& composition, RemoveMask& edit);
Edit apply(Composition& composition, SetMask& edit);
Edit apply(Composition& composition, SetGuides& edit);

nlohmann::json toJson(const SetSliceWarp& e);
nlohmann::json toJson(const SetSoftEdges& e);
nlohmann::json toJson(const SetSliceColor& e);
nlohmann::json toJson(const SetScreenColor& e);
nlohmann::json toJson(const AddMask& e);
nlohmann::json toJson(const RemoveMask& e);
nlohmann::json toJson(const SetMask& e);
nlohmann::json toJson(const SetGuides& e);

std::optional<Edit> mappingEditFromJson(const std::string& op, const nlohmann::json& j);

// Throw ProtocolError for a warp, soft edge or colour out of range.
void checkSliceMapping(const Slice& slice);
void checkColor(const ColorAdjust& color);
void checkMaskRoom(const std::vector<Mask>& masks, size_t adding = 0);
// Checks the masks of a screen or slice being added, gives fresh ids to those without and default names.
void completeMasks(const Composition& composition, const std::vector<Mask*>& masks);

void to_json(nlohmann::json& j, const Warp& warp);
void from_json(const nlohmann::json& j, Warp& warp);
void to_json(nlohmann::json& j, const SoftEdge& softEdge);
void from_json(const nlohmann::json& j, SoftEdge& softEdge);
void to_json(nlohmann::json& j, const ColorAdjust& color);
void from_json(const nlohmann::json& j, ColorAdjust& color);
void to_json(nlohmann::json& j, const Mask& mask);
void from_json(const nlohmann::json& j, Mask& mask);
void to_json(nlohmann::json& j, const Guide& guide);
void from_json(const nlohmann::json& j, Guide& guide);

} // namespace azl
