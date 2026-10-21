#pragma once

#include "document/composition.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace azl {

// Throws ProtocolError when the text clip does not fit its ranges.
void checkText(const TextClip& text);

Edit apply(Composition& composition, SetClipText& edit);
nlohmann::json toJson(const SetClipText& e);
std::optional<Edit> textEditFromJson(const std::string& op, const nlohmann::json& j);

void to_json(nlohmann::json& j, const TextClip& text);
void from_json(const nlohmann::json& j, TextClip& text);

} // namespace azl
