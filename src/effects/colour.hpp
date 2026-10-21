#pragma once

#include "effects/effect_registry.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace azl {

// Straight RGBA, 0..1.
struct Rgba {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 1.0;

    bool operator==(const Rgba&) const = default;
};

// Hue in turns (0..1); saturation, value and alpha 0..1.
struct Hsva {
    double h = 0.0;
    double s = 0.0;
    double v = 0.0;
    double a = 1.0;

    bool operator==(const Hsva&) const = default;
};

Rgba toRgba(const Hsva& colour);
// A grey says nothing about its hue, so it keeps `hue`.
Hsva toHsva(const Rgba& colour, double hue = 0.0);
// "#rrggbb", with alpha "#rrggbbaa".
std::string toHex(const Rgba& colour, bool alpha);
// "#rgb", "#rrggbb" or "#rrggbbaa", with or without "#", in any case; none for anything else.
std::optional<Rgba> fromHex(const std::string& text);
// The colour t (0..1) of the way from `from` to `to`, mixed in OKLab so the blend stays even to the eye; alpha mixes
// straight.
Rgba mixOklab(const Rgba& from, const Rgba& to, double t);
// Lightness and the a, b axes.
std::array<double, 3> toOklab(const Rgba& colour);
// The colour with its OKLab hue turned by `turns`, keeping its lightness and chroma (as far as RGB holds them).
Rgba rotateHue(const Rgba& colour, double turns);

// Parameters of an effect that together make one colour: red, green and blue (`red`, or with a prefix `colorRed`,
// `color1Red`, `toRed`), or hue, saturation and brightness; each 0..1, with an alpha when the effect has one.
enum class ColourModel { Rgb, Hsb };

struct ColourGroup {
    // Its first parameter's name, which names the group within its effect.
    std::string key;
    std::string label;
    ColourModel model = ColourModel::Rgb;
    std::array<std::string, 3> channels;
    // Empty when the group has no alpha.
    std::string alpha;

    bool operator==(const ColourGroup&) const = default;
};

// Found from the parameters' names, in parameter order.
std::vector<ColourGroup> colourGroups(const EffectDef& def);
std::optional<ColourGroup> colourGroup(const EffectDef& def, const std::string& key);
// The group's parameters, alpha last when it has one.
std::vector<std::string> groupParamNames(const ColourGroup& group);
// The colour these values make; a missing parameter is at its default.
Hsva groupColour(const EffectDef& def, const ColourGroup& group, const std::map<std::string, double>& params);
// The values of the group's parameters that make the colour.
std::map<std::string, double> groupParams(const ColourGroup& group, const Hsva& colour);

} // namespace azl
