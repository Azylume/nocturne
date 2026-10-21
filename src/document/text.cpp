#include "document/text_edits.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace azl {

namespace {

constexpr std::array<std::pair<TextAlign, const char*>, 3> kAlignNames{{
    {TextAlign::Left, "left"},
    {TextAlign::Center, "center"},
    {TextAlign::Right, "right"},
}};

constexpr std::array<const char*, kTextParamCount> kTextParamNames = {"textSize", "textX",    "textY",    "textRed",
                                                                      "textGreen", "textBlue", "textAlpha"};

constexpr double kMinTextSize = 0.005;
constexpr double kMaxTextSize = 1.0;
constexpr double kMinPosition = -1.0;
constexpr double kMaxPosition = 2.0;
constexpr size_t kMaxFontName = 256;

const char* alignName(TextAlign align)
{
    return kAlignNames[size_t(align)].second;
}

TextAlign alignFromName(const std::string& name)
{
    for (const auto& [align, n] : kAlignNames)
        if (name == n)
            return align;
    throw ProtocolError("unknown text align " + name);
}

void checkRange(double value, double min, double max, const char* what)
{
    if (!(value >= min && value <= max))
        throw ProtocolError(std::string(what) + " must be within " + std::to_string(min) + ".." + std::to_string(max));
}

void checkColor(const std::array<double, 4>& color, const char* what)
{
    for (double c : color)
        checkRange(c, 0.0, 1.0, what);
}

template <typename T>
std::optional<T> optional(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key))
        return std::nullopt;
    try {
        return j[key].get<T>();
    } catch (const nlohmann::json::exception&) {
        throw ProtocolError(std::string("bad field ") + key);
    }
}

std::optional<TextAlign> alignField(const nlohmann::json& j)
{
    if (auto name = optional<std::string>(j, "align"))
        return alignFromName(*name);
    return std::nullopt;
}

template <typename T>
void swapIfSet(std::optional<T>& value, T& field)
{
    if (value)
        std::swap(*value, field);
}

// Swaps every set field of `edit` with the clip's, so the edit then holds what the clip had.
void swapFields(SetClipText& edit, TextClip& text)
{
    swapIfSet(edit.text, text.text);
    swapIfSet(edit.font, text.font);
    swapIfSet(edit.size, text.size);
    swapIfSet(edit.color, text.color);
    swapIfSet(edit.align, text.align);
    swapIfSet(edit.x, text.x);
    swapIfSet(edit.y, text.y);
    swapIfSet(edit.lineSpacing, text.lineSpacing);
    swapIfSet(edit.outline, text.outline);
    swapIfSet(edit.outlineColor, text.outlineColor);
}

} // namespace

std::optional<TextParam> textParamFromName(const std::string& name)
{
    auto it = std::find(kTextParamNames.begin(), kTextParamNames.end(), name);
    return it == kTextParamNames.end() ? std::nullopt : std::optional(TextParam(it - kTextParamNames.begin()));
}

std::pair<double, double> textParamRange(TextParam param)
{
    switch (param) {
    case TextParam::Size: return {kMinTextSize, kMaxTextSize};
    case TextParam::X:
    case TextParam::Y: return {kMinPosition, kMaxPosition};
    default: return {0.0, 1.0};
    }
}

double textParamValue(const TextClip& clip, TextParam param)
{
    switch (param) {
    case TextParam::Size: return clip.size;
    case TextParam::X: return clip.x;
    case TextParam::Y: return clip.y;
    case TextParam::Red: return clip.color[0];
    case TextParam::Green: return clip.color[1];
    case TextParam::Blue: return clip.color[2];
    case TextParam::Alpha: return clip.color[3];
    }
    return 0.0;
}

void checkText(const TextClip& text)
{
    if (text.text.size() > kMaxTextLength)
        throw ProtocolError("text is longer than " + std::to_string(kMaxTextLength) + " bytes");
    if (text.font.empty() || text.font.size() > kMaxFontName)
        throw ProtocolError("font must be a name of 1..256 bytes");
    checkRange(text.size, kMinTextSize, kMaxTextSize, "text size");
    checkRange(text.x, kMinPosition, kMaxPosition, "text x");
    checkRange(text.y, kMinPosition, kMaxPosition, "text y");
    checkRange(text.lineSpacing, 0.5, 4.0, "line spacing");
    checkRange(text.outline, 0.0, 0.5, "outline");
    checkColor(text.color, "text color");
    checkColor(text.outlineColor, "outline color");
}

Edit apply(Composition& composition, SetClipText& edit)
{
    Layer* layer = nullptr;
    forEachLayer(composition.layers, [&](Layer& l) {
        if (l.id == edit.layer)
            layer = &l;
    });
    if (!layer)
        throw ProtocolError("no layer " + std::to_string(edit.layer));
    auto cell = layer->clips.find(edit.column);
    if (cell == layer->clips.end() || !cell->second.text)
        throw ProtocolError("layer " + std::to_string(edit.layer) + " has no text clip in column " +
                            std::to_string(edit.column));
    TextClip text = *cell->second.text;
    SetClipText inverse = edit;
    swapFields(inverse, text);
    checkText(text);
    cell->second.text = std::move(text);
    return inverse;
}

nlohmann::json toJson(const SetClipText& e)
{
    nlohmann::json j{{"op", "setClipText"}, {"layer", e.layer}, {"column", e.column}};
    if (e.text)
        j["text"] = *e.text;
    if (e.font)
        j["font"] = *e.font;
    if (e.size)
        j["size"] = *e.size;
    if (e.color)
        j["color"] = *e.color;
    if (e.align)
        j["align"] = alignName(*e.align);
    if (e.x)
        j["x"] = *e.x;
    if (e.y)
        j["y"] = *e.y;
    if (e.lineSpacing)
        j["lineSpacing"] = *e.lineSpacing;
    if (e.outline)
        j["outline"] = *e.outline;
    if (e.outlineColor)
        j["outlineColor"] = *e.outlineColor;
    return j;
}

std::optional<Edit> textEditFromJson(const std::string& op, const nlohmann::json& j)
{
    if (op != "setClipText")
        return std::nullopt;
    const auto layer = optional<uint64_t>(j, "layer");
    const auto column = optional<uint64_t>(j, "column");
    if (!layer || !column)
        throw ProtocolError(std::string("missing field ") + (layer ? "column" : "layer"));
    return SetClipText{*layer,
                       *column,
                       optional<std::string>(j, "text"),
                       optional<std::string>(j, "font"),
                       optional<double>(j, "size"),
                       optional<std::array<double, 4>>(j, "color"),
                       alignField(j),
                       optional<double>(j, "x"),
                       optional<double>(j, "y"),
                       optional<double>(j, "lineSpacing"),
                       optional<double>(j, "outline"),
                       optional<std::array<double, 4>>(j, "outlineColor")};
}

void to_json(nlohmann::json& j, const TextClip& text)
{
    j = {{"text", text.text},
         {"font", text.font},
         {"size", text.size},
         {"color", text.color},
         {"align", alignName(text.align)},
         {"x", text.x},
         {"y", text.y},
         {"lineSpacing", text.lineSpacing},
         {"outline", text.outline},
         {"outlineColor", text.outlineColor}};
    if (!text.links.empty())
        j["links"] = text.links;
}

// Every field is optional.
void from_json(const nlohmann::json& j, TextClip& text)
{
    if (!j.is_object())
        throw ProtocolError("bad field text");
    nlohmann::json edit = j;
    edit["layer"] = 0;
    edit["column"] = 0;
    SetClipText fields = std::get<SetClipText>(*textEditFromJson("setClipText", edit));
    text = {};
    swapFields(fields, text);
    text.links = j.value("links", std::map<std::string, uint64_t>{});
}

} // namespace azl
