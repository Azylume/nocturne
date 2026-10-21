#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace azl {

enum class TextAlign { Left, Center, Right };

// A text generator: a clip that renders a string instead of playing media. `size` is the font size as a fraction of
// the composition's height. The text block is anchored at x, y (the composition's 0..1 space, y down): its left edge,
// centre or right edge by `align`, centred vertically. Lines break only at newlines, `lineSpacing` times the font's
// line height apart. `outline` is the outline's width as a fraction of the font size, drawn under the fill. Colours
// are straight RGBA, 0..1.
struct TextClip {
    std::string text;
    std::string font = "Helvetica";
    double size = 0.1;
    std::array<double, 4> color{1.0, 1.0, 1.0, 1.0};
    TextAlign align = TextAlign::Center;
    double x = 0.5;
    double y = 0.5;
    double lineSpacing = 1.0;
    double outline = 0.0;
    std::array<double, 4> outlineColor{0.0, 0.0, 0.0, 1.0};
    // "color" and "outlineColor" follow these colours of the active palette, by colour id.
    std::map<std::string, uint64_t> links{};

    bool operator==(const TextClip&) const = default;
};

inline constexpr size_t kMaxTextLength = 4096;

// Changes only the fields that are set, of the text clip in the cell.
struct SetClipText {
    uint64_t layer = 0;
    uint64_t column = 0;
    std::optional<std::string> text{};
    std::optional<std::string> font{};
    std::optional<double> size{};
    std::optional<std::array<double, 4>> color{};
    std::optional<TextAlign> align{};
    std::optional<double> x{};
    std::optional<double> y{};
    std::optional<double> lineSpacing{};
    std::optional<double> outline{};
    std::optional<std::array<double, 4>> outlineColor{};
};

// What rasterising a text clip depends on; colour, position and modulated size are applied when it is drawn.
struct TextLayout {
    std::string text;
    std::string font;
    double size = 0.1;
    TextAlign align = TextAlign::Center;
    double lineSpacing = 1.0;
    double outline = 0.0;

    auto operator<=>(const TextLayout&) const = default;
};

inline TextLayout layoutOf(const TextClip& clip)
{
    return {clip.text, clip.font, clip.size, clip.align, clip.lineSpacing, clip.outline};
}

// The text parameters macros and modulators drive, as a layer's own parameters (effect 0): they act on whichever text
// clip the layer plays.
enum class TextParam { Size, X, Y, Red, Green, Blue, Alpha };

inline constexpr size_t kTextParamCount = 7;

std::optional<TextParam> textParamFromName(const std::string& name);
std::pair<double, double> textParamRange(TextParam param);
double textParamValue(const TextClip& clip, TextParam param);

} // namespace azl
