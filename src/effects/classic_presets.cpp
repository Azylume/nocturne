#include "effects/classic_presets.hpp"

#include "effects/effect_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

namespace azl {

namespace {

namespace fs = std::filesystem;

// "Direction Spread" -> "directionSpread", "Color.Red" -> "colorRed", "RGB Frequency" -> "rgbFrequency".
std::string camel(const std::string& name)
{
    std::vector<std::string> words(1);
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)))
            words.back() += c;
        else if (!words.back().empty())
            words.emplace_back();
    std::string out;
    for (std::string word : words) {
        if (word.empty())
            continue;
        if (!out.empty())
            word[0] = char(std::toupper(static_cast<unsigned char>(word[0])));
        else if (std::none_of(word.begin(), word.end(), [](unsigned char c) { return std::islower(c); }))
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        else
            word[0] = char(std::tolower(static_cast<unsigned char>(word[0])));
        out += word;
    }
    return out;
}

const ParamDef* paramOf(const EffectDef& def, const std::string& id)
{
    auto it = std::find_if(def.params.begin(), def.params.end(), [&](const ParamDef& p) { return p.name == id; });
    return it == def.params.end() ? nullptr : &*it;
}

bool animated(const XmlNode& param)
{
    return std::any_of(param.children.begin(), param.children.end(), [](const auto& c) {
        return c->name.starts_with("PhaseSource") && c->name != "PhaseSourceStatic";
    });
}

void setParam(const EffectDef& def, const std::string& name, const XmlNode& param, ClassicPreset& out)
{
    const ParamDef* p = paramOf(def, camel(name));
    char* end = nullptr;
    const std::string text = param.attr("value");
    const double value = std::strtod(text.c_str(), &end);
    if (!p || text.empty() || *end) {
        out.skipped.push_back(name);
        return;
    }
    out.params[p->name] = fitParam(*p, value);
    if (animated(param))
        out.skipped.push_back(name + " animation");
}

void setParams(const EffectDef& def, const XmlNode* params, ClassicPreset& out)
{
    static const std::set<std::string> derivedChannels{"Hue", "Saturation", "Brightness"};
    if (!params)
        return;
    for (const auto& param : params->children) {
        const std::string name = param->attr("name");
        if (!param->name.starts_with("Param") || name == "Bypassed")
            continue;
        if (param->name != "ParamColor") {
            setParam(def, name, *param, out);
            continue;
        }
        if (const XmlNode* channels = param->child("Params", "Channels"))
            for (const auto& channel : channels->children)
                if (!derivedChannels.contains(channel->attr("name")))
                    setParam(def, name + "." + channel->attr("name"), *channel, out);
    }
}

} // namespace

std::optional<ClassicPreset> classicPreset(const XmlNode& preset, const std::string& name)
{
    static const std::map<std::string, std::string> types{
        {"Stripper", "classicStripper"},
        {"Colorize", "classicColorize"},
        {"ShiftRGB", "classicShiftRgb"},
        {"Mirror", "classicMirror"},
        {"Kaleidoscope", "classicKaleidoscope"},
        {"TwitchEffect", "classicTwitch"},
        {"AutoMask", "classicAutoMask"},
        {"BrightnessContrast", "classicBrightnessContrast"},
        {"Levels", "classicLevels"},
        {"Threshold", "classicThreshold"},
        {"HueRotate", "classicHueRotate"},
        {"MirrorQuad", "classicMirrorQuad"},
        {"PolarKaleido", "classicPolarKaleido"},
        {"Trails", "classicTrails"},
        {"Strobe", "classicStrobe"},
        {"ShiftGlitch", "classicShiftGlitch"},
        {"Shift Glitch", "classicShiftGlitch"},
        {"EdgeDetection", "classicEdgeDetection"}};
    const XmlNode* wrapper = preset.child("RenderPass");
    const XmlNode* inner = wrapper ? wrapper->child("RenderPass") : nullptr;
    if (!inner)
        return std::nullopt;
    auto type = types.find(inner->attr("type") == "FFGLEffect" ? inner->attr("name") : inner->attr("type"));
    const EffectDef* def = type == types.end() ? nullptr : findEffect(type->second);
    if (!def)
        return std::nullopt;
    ClassicPreset out{.effectType = def->type, .name = name};
    setParams(*def, inner->child("Params"), out);
    const XmlNode* wrapperParams = wrapper->child("Params");
    if (const XmlNode* opacity = wrapperParams ? wrapperParams->child("ParamRange", "Opacity") : nullptr)
        setParam(*def, "Mix", *opacity, out);
    const XmlNode* mixer = wrapper->at("ChoosableMixer/RenderPass");
    const std::string blend = mixer ? mixer->attr("type") : "Alpha";
    if (blend == "Add" && paramOf(*def, "blendMode"))
        out.params["blendMode"] = 1.0;
    else if (blend != "Alpha")
        out.skipped.push_back(blend + " mixer");
    return out;
}

std::vector<ClassicPreset> readClassicPresets(const std::vector<fs::path>& roots)
{
    std::vector<ClassicPreset> presets;
    std::set<std::pair<std::string, std::string>> seen;
    for (const fs::path& root : roots) {
        std::error_code ec;
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(root / "Video Effects", ec), end; !ec && it != end; it.increment(ec))
            if (it.depth() == 1 && it->path().extension() == ".xml")
                files.push_back(it->path());
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            std::optional<ClassicPreset> preset;
            try {
                preset = classicPreset(*parseXmlFile(file.string()), file.stem().string());
            } catch (const std::exception&) {
                continue;
            }
            if (preset && seen.insert({preset->effectType, preset->name}).second)
                presets.push_back(std::move(*preset));
        }
    }
    return presets;
}

std::vector<ClassicPreset> classicBuiltinPresets()
{
    const std::vector<ClassicPreset> looks = {
        {"classicStripper", "Two-Way Bars", {{"number", 0.543}, {"movementType", 0.773}, {"globalWidth", 0.155}}},
        {"classicStripper", "Crossed Bars",
         {{"number", 0.698}, {"mode", 0.467}, {"directionSpread", 0.333}, {"movementType", 0.773},
          {"globalWidth", 0.155}}},
        {"classicStripper", "Negative Bars",
         {{"number", 0.698}, {"mode", 0.842}, {"movementType", 0.773}, {"globalWidth", 0.155}}},
        {"classicStripper", "Tilted Bars", {{"number", 0.543}, {"direction", 0.364}, {"globalWidth", 0.155}}},
        {"classicStripper", "Wide Bars", {{"number", 0.282}, {"globalWidth", 0.32}}},
        {"classicStripper", "Zoomed Bars",
         {{"number", 0.698}, {"mode", 0.701}, {"direction", 0.248}, {"movementType", 0.773}, {"globalWidth", 0.313},
          {"feather", 0.89}}},
        {"classicColorize", "Cobalt", {{"colorRed", 0.0}, {"colorGreen", 0.09}, {"colorBlue", 1.0}, {"blendMode", 1.0}}},
        {"classicColorize", "Fuchsia", {{"colorBlue", 0.933}, {"blendMode", 1.0}}},
        {"classicColorize", "Spectrum Map", {{"colorBlue", 1.0}, {"mode", 1.0}}},
        {"classicTwitch", "Jolt",
         {{"maxPosX", 0.2}, {"posFrequency", 0.33}, {"maxLight", 0.75}, {"lightFrequency", 0.33}, {"maxRGBShift", 1.0},
          {"rgbFrequency", 0.25}}},
        {"classicTwitch", "Mayhem",
         {{"maxPosX", 0.7}, {"maxPosY", 0.7}, {"posFrequency", 0.553}, {"maxScale", 0.5}, {"scaleFrequency", 0.25},
          {"maxOpacity", 0.75}, {"maxLight", 0.9}, {"lightFrequency", 0.8}, {"maxRGBShift", 0.696},
          {"rgbFrequency", 0.45}, {"maxBlur", 0.598}, {"blurFrequency", 0.5}}},
        {"classicTwitch", "Flicker",
         {{"maxPosX", 0.3}, {"posFrequency", 0.125}, {"maxLight", 0.25}, {"lightFrequency", 0.2}, {"maxRGBShift", 0.25},
          {"rgbFrequency", 0.15}, {"maxBlur", 0.5}, {"blurFrequency", 0.2}}},
        {"classicShiftRgb", "Anaglyph", {{"distance", 0.0304}, {"r", -0.2}, {"g", 0.2}, {"blendMode", 1.0}}},
        {"classicShiftRgb", "Prism Zoom",
         {{"mode", 5.0}, {"r", 0.0829}, {"g", -0.0186}, {"b", 0.2217}, {"blendMode", 1.0}}},
        {"classicShiftRgb", "Fringe", {{"distance", 0.4834}, {"r", -0.2}, {"b", 0.2}, {"blendMode", 1.0}}},
        {"classicShiftRgb", "Channel Twist",
         {{"distance", 0.2735}, {"mode", 4.0}, {"r", -0.2}, {"b", 0.2}, {"blendMode", 1.0}}},
        {"classicShiftRgb", "Vertical Fringe",
         {{"distance", 0.2517}, {"mode", 1.0}, {"r", -0.2}, {"b", 0.2}, {"blendMode", 1.0}}},
        {"classicMirror", "Side Mirror", {{"x", 0.5}, {"flipX", 1.0}}},
        {"classicMirror", "Tunnel", {{"x", 0.5}, {"y", 0.5}, {"inOut", 0.1977}, {"blendMode", 1.0}}},
        {"classicMirror", "Upper Mirror", {{"y", 0.5}, {"inOut", 0.7473}}},
        {"classicKaleidoscope", "Nine Petals",
         {{"angles", 0.775}, {"zoom", 0.578}, {"inputRotation", 0.308}, {"blendMode", 1.0}}},
        {"classicKaleidoscope", "Eight Petals",
         {{"angles", 0.681}, {"zoom", 0.578}, {"inputRotation", 0.987}, {"blendMode", 1.0}}},
        {"classicShiftGlitch", "Row Slips", {{"horizontal", 0.0822}, {"size", 0.3484}, {"blendMode", 1.0}}},
        {"classicShiftGlitch", "Shattered",
         {{"vertical", 1.0}, {"horizontal", 1.0}, {"size", 1.0}, {"blendMode", 1.0}}},
        {"classicShiftGlitch", "Scan Lines", {{"horizontal", 0.5995}, {"size", 0.0081}, {"blendMode", 1.0}}},
        {"classicShiftGlitch", "Column Slips",
         {{"vertical", 0.085}, {"horizontal", 0.0}, {"size", 0.2125}, {"blendMode", 1.0}}},
    };
    std::vector<ClassicPreset> out;
    for (ClassicPreset look : looks) {
        if (look.effectType == "classicStripper")
            look.params.emplace("blendMode", 1.0);
        for (const ParamDef& param : findEffect(look.effectType)->params)
            look.params.emplace(param.name, param.defaultValue);
        out.push_back(std::move(look));
    }
    return out;
}

std::vector<fs::path> classicPresetRoots()
{
    const char* home = std::getenv("HOME");
    std::vector<fs::path> roots;
    for (const auto& [dir, presets] : {std::pair<fs::path, fs::path>{fs::path(home ? home : "/") / "Documents", "Presets"},
                                       {"/Applications", "default/Presets"}}) {
        std::vector<fs::path> found;
        std::error_code ec, isDir;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (fs::is_directory(it->path() / presets / "Video Effects", isDir))
                found.push_back(it->path() / presets);
        std::sort(found.begin(), found.end());
        roots.insert(roots.end(), found.begin(), found.end());
    }
    return roots;
}

} // namespace azl
