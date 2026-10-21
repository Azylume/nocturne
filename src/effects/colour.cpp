#include "effects/colour.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace azl {

Rgba toRgba(const Hsva& c)
{
    auto channel = [&](double offset) {
        const double k = std::fmod(offset + c.h * 6.0, 6.0);
        const double k6 = k < 0.0 ? k + 6.0 : k;
        return c.v - c.v * c.s * std::clamp(std::min(k6, 4.0 - k6), 0.0, 1.0);
    };
    return {channel(5.0), channel(3.0), channel(1.0), c.a};
}

Hsva toHsva(const Rgba& c, double hue)
{
    const double hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
    const double d = hi - lo;
    Hsva out{hue, hi > 0.0 ? d / hi : 0.0, hi, c.a};
    if (d <= 1e-9)
        return out;
    double h = hi == c.r ? (c.g - c.b) / d : hi == c.g ? 2.0 + (c.b - c.r) / d : 4.0 + (c.r - c.g) / d;
    h /= 6.0;
    out.h = h < 0.0 ? h + 1.0 : h;
    return out;
}

namespace {

int byte(double v)
{
    return int(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

std::optional<int> hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = char(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return std::nullopt;
}

bool endsWith(const std::string& text, const std::string& end)
{
    return text.size() > end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

// "color1" reads "Color 1", "to" reads "To".
std::string prefixLabel(const std::string& prefix)
{
    std::string label;
    for (size_t i = 0; i < prefix.size(); ++i) {
        const char c = prefix[i];
        if (std::isdigit(static_cast<unsigned char>(c)) && i > 0 && !std::isdigit(static_cast<unsigned char>(prefix[i - 1])))
            label += ' ';
        label += i == 0 ? char(std::toupper(static_cast<unsigned char>(c))) : c;
    }
    return label;
}

const ParamDef* paramNamed(const EffectDef& def, const std::string& name)
{
    auto it = std::find_if(def.params.begin(), def.params.end(), [&](const ParamDef& p) { return p.name == name; });
    return it == def.params.end() ? nullptr : &*it;
}

bool unitRange(const EffectDef& def, const std::string& name)
{
    const ParamDef* p = paramNamed(def, name);
    return p && p->min == 0.0 && p->max == 1.0;
}

std::optional<ColourGroup> rgbGroup(const EffectDef& def, const ParamDef& red)
{
    if (red.name != "red" && !endsWith(red.name, "Red"))
        return std::nullopt;
    const std::string prefix = red.name == "red" ? "" : red.name.substr(0, red.name.size() - 3);
    auto named = [&](const char* lower, const char* upper) { return prefix.empty() ? std::string(lower) : prefix + upper; };
    ColourGroup group{red.name, "", ColourModel::Rgb, {red.name, named("green", "Green"), named("blue", "Blue")}};
    for (const std::string& channel : group.channels)
        if (!unitRange(def, channel))
            return std::nullopt;
    if (unitRange(def, named("alpha", "Alpha")))
        group.alpha = named("alpha", "Alpha");
    if (endsWith(red.label, " Red"))
        group.label = red.label.substr(0, red.label.size() - 4);
    else
        group.label = prefix.empty() ? "Colour" : prefixLabel(prefix);
    return group;
}

std::optional<ColourGroup> hsbGroup(const EffectDef& def, const ParamDef& hue)
{
    if (hue.name != "hue" || !unitRange(def, "hue") || !unitRange(def, "saturation") || !unitRange(def, "brightness"))
        return std::nullopt;
    return ColourGroup{"hue", "Colour", ColourModel::Hsb, {"hue", "saturation", "brightness"},
                       unitRange(def, "alpha") ? "alpha" : ""};
}

} // namespace

std::string toHex(const Rgba& c, bool alpha)
{
    char text[10];
    if (alpha)
        std::snprintf(text, sizeof text, "#%02x%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
    else
        std::snprintf(text, sizeof text, "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
    return text;
}

std::optional<Rgba> fromHex(const std::string& text)
{
    std::string digits = text;
    digits.erase(std::remove_if(digits.begin(), digits.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }),
                 digits.end());
    if (!digits.empty() && digits[0] == '#')
        digits.erase(0, 1);
    if (digits.size() == 3)
        digits = {digits[0], digits[0], digits[1], digits[1], digits[2], digits[2]};
    if (digits.size() != 6 && digits.size() != 8)
        return std::nullopt;
    std::array<double, 4> v{0.0, 0.0, 0.0, 1.0};
    for (size_t i = 0; i < digits.size() / 2; ++i) {
        const auto hi = hexDigit(digits[i * 2]), lo = hexDigit(digits[i * 2 + 1]);
        if (!hi || !lo)
            return std::nullopt;
        v[i] = (*hi * 16 + *lo) / 255.0;
    }
    return Rgba{v[0], v[1], v[2], v[3]};
}

std::vector<ColourGroup> colourGroups(const EffectDef& def)
{
    std::vector<ColourGroup> groups;
    for (const ParamDef& param : def.params) {
        if (auto group = rgbGroup(def, param))
            groups.push_back(*group);
        else if (auto hsb = hsbGroup(def, param))
            groups.push_back(*hsb);
    }
    return groups;
}

std::optional<ColourGroup> colourGroup(const EffectDef& def, const std::string& key)
{
    for (const ColourGroup& group : colourGroups(def))
        if (group.key == key)
            return group;
    return std::nullopt;
}

std::vector<std::string> groupParamNames(const ColourGroup& group)
{
    std::vector<std::string> names(group.channels.begin(), group.channels.end());
    if (!group.alpha.empty())
        names.push_back(group.alpha);
    return names;
}

Hsva groupColour(const EffectDef& def, const ColourGroup& group, const std::map<std::string, double>& params)
{
    auto value = [&](const std::string& name) {
        if (auto it = params.find(name); it != params.end())
            return it->second;
        const ParamDef* p = paramNamed(def, name);
        return p ? p->defaultValue : 1.0;
    };
    const double a = group.alpha.empty() ? 1.0 : value(group.alpha);
    const auto& c = group.channels;
    if (group.model == ColourModel::Hsb)
        return {value(c[0]), value(c[1]), value(c[2]), a};
    return toHsva({value(c[0]), value(c[1]), value(c[2]), a});
}

std::map<std::string, double> groupParams(const ColourGroup& group, const Hsva& colour)
{
    const auto& c = group.channels;
    std::map<std::string, double> params;
    if (group.model == ColourModel::Hsb) {
        params = {{c[0], colour.h}, {c[1], colour.s}, {c[2], colour.v}};
    } else {
        const Rgba rgb = toRgba(colour);
        params = {{c[0], rgb.r}, {c[1], rgb.g}, {c[2], rgb.b}};
    }
    if (!group.alpha.empty())
        params[group.alpha] = colour.a;
    return params;
}

} // namespace azl

namespace azl {

namespace {

double toLinear(double c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double toEncoded(double c)
{
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

} // namespace

std::array<double, 3> toOklab(const Rgba& c)
{
    const double r = toLinear(c.r), g = toLinear(c.g), b = toLinear(c.b);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

namespace {

Rgba fromOklab(const std::array<double, 3>& lab, double alpha)
{
    const double l = std::pow(lab[0] + 0.3963377774 * lab[1] + 0.2158037573 * lab[2], 3.0);
    const double m = std::pow(lab[0] - 0.1055613458 * lab[1] - 0.0638541728 * lab[2], 3.0);
    const double s = std::pow(lab[0] - 0.0894841775 * lab[1] - 1.2914855480 * lab[2], 3.0);
    auto channel = [](double v) { return std::clamp(toEncoded(v), 0.0, 1.0); };
    return {channel(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
            channel(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
            channel(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s), alpha};
}

} // namespace

Rgba mixOklab(const Rgba& from, const Rgba& to, double t)
{
    const std::array<double, 3> a = toOklab(from), b = toOklab(to);
    std::array<double, 3> mixed{};
    for (size_t i = 0; i < 3; ++i)
        mixed[i] = a[i] + (b[i] - a[i]) * t;
    return fromOklab(mixed, from.a + (to.a - from.a) * t);
}

Rgba rotateHue(const Rgba& colour, double turns)
{
    const std::array<double, 3> lab = toOklab(colour);
    const double c = std::cos(turns * 2.0 * M_PI), s = std::sin(turns * 2.0 * M_PI);
    return fromOklab({lab[0], lab[1] * c - lab[2] * s, lab[1] * s + lab[2] * c}, colour.a);
}

} // namespace azl
