#include "document/composition_file.hpp"
#include "document/palette.hpp"
#include "effects/colour.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <doctest.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using azl::ColourGroup;
using azl::ColourModel;

namespace {

std::vector<std::string> keys(const std::string& type)
{
    std::vector<std::string> out;
    for (const ColourGroup& group : azl::colourGroups(*azl::findEffect(type)))
        out.push_back(group.key);
    return out;
}

} // namespace

TEST_CASE("colour: the colour groups are found from the parameter names of every effect that has one")
{
    const std::map<std::string, std::vector<std::string>> expected{
        {"colorize", {"hue"}},
        {"stripper", {"red"}},
        {"classicStripper", {"red"}},
        {"classicColorize", {"colorRed"}},
        {"classicThreshold", {"color1Red", "color2Red"}},
        {"classicStrobe", {"colorRed"}},
        {"classicEdgeDetection", {"colorRed"}},
        {"solid", {"red"}},
        {"strobe", {"red"}},
        {"lines", {"red"}},
        {"checker", {"red"}},
        {"rings", {"red"}},
        {"gradient", {"red", "toRed"}},
        {"noise", {"red"}},
        {"isfTriangleMosaic", {"outlineColorRed"}},
        {"isfGraphPaper", {"bgColorRed", "lineColorRed"}},
        {"isfSineWarpGradient", {"xcolorRed", "ycolorRed", "backgroundRed"}},
        {"isfSpiral", {"color1Red", "color2Red"}},
        {"isfStripes", {"color1Red", "color2Red"}},
        {"isfWorleyCells", {"cellColor1Red", "cellColor2Red", "borderColorRed"}},
    };
    std::vector<std::string> types = azl::effectTypes();
    for (const std::string& type : azl::generatorTypes())
        types.push_back(type);
    for (const std::string& type : types) {
        auto it = expected.find(type);
        CHECK_MESSAGE(keys(type) == (it == expected.end() ? std::vector<std::string>{} : it->second), type);
    }

    const ColourGroup threshold = *azl::colourGroup(*azl::findEffect("classicThreshold"), "color2Red");
    CHECK(threshold.label == "Color 2");
    CHECK(threshold.alpha == "color2Alpha");
    const ColourGroup to = *azl::colourGroup(*azl::findEffect("gradient"), "toRed");
    CHECK(to.label == "To");
    CHECK(to.alpha.empty());
    const ColourGroup colorize = *azl::colourGroup(*azl::findEffect("colorize"), "hue");
    CHECK(colorize.model == ColourModel::Hsb);
    CHECK(colorize.alpha == "alpha");
    CHECK(azl::colourGroup(*azl::findEffect("solid"), "red")->label == "Colour");
}

TEST_CASE("colour: hex, HSV and the group values round-trip")
{
    CHECK(azl::toHex({1.0, 0.5, 0.0, 1.0}, false) == "#ff8000");
    CHECK(azl::toHex({1.0, 0.5, 0.0, 0.5}, true) == "#ff800080");
    CHECK(*azl::fromHex("#F80") == azl::Rgba{1.0, 0x88 / 255.0, 0.0, 1.0});
    CHECK(azl::fromHex("00000000")->a == 0.0);
    CHECK_FALSE(azl::fromHex("#12345"));
    CHECK_FALSE(azl::fromHex("#gg0000"));

    for (const azl::Rgba c : {azl::Rgba{1, 0, 0, 1}, azl::Rgba{0.2, 0.4, 0.6, 0.5}, azl::Rgba{0.9, 0.9, 0.1, 1}}) {
        const azl::Rgba back = azl::toRgba(azl::toHsva(c));
        CHECK(back.r == doctest::Approx(c.r));
        CHECK(back.g == doctest::Approx(c.g));
        CHECK(back.b == doctest::Approx(c.b));
        CHECK(back.a == c.a);
    }
    CHECK(azl::toHsva({0.5, 0.5, 0.5, 1}, 0.3).h == 0.3);

    const azl::EffectDef& threshold = *azl::findEffect("classicThreshold");
    const ColourGroup group = *azl::colourGroup(threshold, "color1Red");
    const std::map<std::string, double> params = azl::groupParams(group, azl::toHsva({0.25, 0.5, 0.75, 0.4}));
    CHECK(params.size() == 4);
    CHECK(params.at("color1Green") == doctest::Approx(0.5));
    CHECK(params.at("color1Alpha") == doctest::Approx(0.4));
    CHECK(azl::toRgba(azl::groupColour(threshold, group, params)).b == doctest::Approx(0.75));
    // Missing parameters read their defaults: Color 1 is white.
    CHECK(azl::toRgba(azl::groupColour(threshold, group, {})) == azl::Rgba{1, 1, 1, 1});
}

TEST_CASE("colour: a colour in the document reads and edits as its effect's parameters or its text clip's colour")
{
    azl::Composition c = azl::defaultComposition();
    azl::Edit add = azl::AddEffect{1, std::nullopt, {0, "classicStrobe", {}}};
    azl::applyEdit(c, add);
    const uint64_t strobe = std::get<azl::AddEffect>(add).effect.id;
    const azl::ColourRef ref{strobe, "colorRed"};
    CHECK(azl::toRgba(*azl::colourOf(c, ref)) == azl::Rgba{1, 1, 1, 1});
    CHECK(azl::hasAlpha(c, ref));
    CHECK(azl::colourLabel(c, ref) == "Color");
    CHECK(azl::colourParams(c, ref).size() == 4);
    azl::Edit edit = *azl::colourEdit(c, ref, azl::toHsva({0.0, 1.0, 0.0, 0.5}));
    azl::applyEdit(c, edit);
    CHECK(azl::effectById(c, strobe)->params.at("colorGreen") == 1.0);
    CHECK(azl::effectById(c, strobe)->params.at("colorRed") == 0.0);
    CHECK(azl::effectById(c, strobe)->params.at("colorAlpha") == 0.5);
    CHECK_FALSE(azl::colourOf(c, {strobe, "red"}));

    azl::Edit text = azl::SetSlot{1, 1, azl::ClipRef{.name = "Text", .text = azl::TextClip{.text = "hi"}}};
    azl::applyEdit(c, text);
    const azl::ColourRef outline{0, azl::kTextOutline, 1, 1};
    CHECK(azl::toRgba(*azl::colourOf(c, outline)) == azl::Rgba{0, 0, 0, 1});
    CHECK(azl::colourParams(c, outline).empty());
    azl::Edit recolour = *azl::colourEdit(c, outline, azl::toHsva({1, 0, 0, 1}));
    azl::applyEdit(c, recolour);
    CHECK(c.layers[0].clips.at(1).text->outlineColor == std::array<double, 4>{1, 0, 0, 1});
}

namespace {

azl::ColourPalette flyer()
{
    return {0, "Flyer", {{"green", {0.2, 0.9, 0.3, 1.0}}, {"purple", {0.5, 0.1, 0.7, 1.0}}, {"bone", {0.9, 0.88, 0.8, 1.0}}}};
}

std::string ctl(uint16_t port, const std::string& args)
{
    const fs::path binary = ctlPath();
    const std::string command = "'" + binary.string() + "' --port " + std::to_string(port) + " " + args + " 2>&1";
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe))
        out += buffer;
    CHECK(pclose(pipe) == 0);
    return out;
}

} // namespace

TEST_CASE("palette: palettes are added, replaced and removed as undoable edits, and limits hold")
{
    azl::Composition c = azl::defaultComposition();
    azl::Edit add = azl::SetPalette{flyer()};
    azl::Edit undoAdd = azl::applyEdit(c, add);
    REQUIRE(c.palettes.size() == 1);
    CHECK(c.palettes[0].id == 1);
    CHECK(std::get<azl::RemovePalette>(undoAdd).palette == 1);
    CHECK(azl::activePalette(c) == &c.palettes[0]);
    const azl::ColourPalette first = c.palettes[0];
    CHECK(first.colours[0].id == 1);
    CHECK(first.colours[2].id == 3);

    azl::ColourPalette renamed = c.palettes[0];
    renamed.name = "Night";
    renamed.colours[1].colour = {1.0, 0.0, 0.0, 1.0};
    azl::Edit replace = azl::SetPalette{renamed};
    azl::Edit undoReplace = azl::applyEdit(c, replace);
    CHECK(c.palettes.size() == 1);
    CHECK(c.palettes[0].name == "Night");
    azl::applyEdit(c, undoReplace);
    CHECK(c.palettes[0] == first);

    azl::Edit remove = azl::RemovePalette{1};
    azl::Edit undoRemove = azl::applyEdit(c, remove);
    CHECK(c.palettes.empty());
    azl::applyEdit(c, undoRemove);
    CHECK(c.palettes.size() == 1);

    azl::ColourPalette bad = flyer();
    bad.colours[0].colour[2] = 1.5;
    azl::Edit badEdit = azl::SetPalette{bad};
    CHECK_THROWS_AS(azl::applyEdit(c, badEdit), azl::ProtocolError);
    azl::Edit missing = azl::RemovePalette{9};
    CHECK_THROWS_AS(azl::applyEdit(c, missing), azl::ProtocolError);

    const azl::SetPalette added = azl::addToPalette(c, {"red", {1, 0, 0, 1}});
    CHECK(added.palette.id == 1);
    CHECK(added.palette.colours.size() == 4);
    CHECK(azl::nextColourName(&c.palettes[0]) == "Colour 4");
    CHECK(azl::addToPalette(azl::defaultComposition(), {"red", {1, 0, 0, 1}}).palette.id == 0);

    // A palette colour reads and edits as a colour.
    const azl::ColourRef slot{.group = azl::kPaletteSlot, .palette = 1, .slot = 2};
    CHECK(azl::colourLabel(c, slot) == "bone");
    CHECK(azl::hasAlpha(c, slot));
    azl::Edit recolour = *azl::colourEdit(c, slot, azl::toHsva({0, 0, 1, 1}));
    azl::applyEdit(c, recolour);
    CHECK(c.palettes[0].colours[2].colour == std::array<double, 4>{0, 0, 1, 1});
    CHECK_FALSE(azl::colourOf(c, {.group = azl::kPaletteSlot, .palette = 1, .slot = 3}));
}

TEST_CASE("palette: the palette is saved with the show, edits travel as JSON, and an old file loads with none")
{
    azl::Composition c = azl::defaultComposition();
    azl::Edit add = azl::SetPalette{flyer()};
    azl::applyEdit(c, add);
    CHECK(azl::editFromJson(azl::editToJson(add)).index() == add.index());
    CHECK(std::get<azl::SetPalette>(azl::editFromJson(azl::editToJson(add))).palette == c.palettes[0]);
    CHECK(std::get<azl::RemovePalette>(azl::editFromJson({{"op", "removePalette"}, {"palette", 1}})).palette == 1);

    TempDir dir;
    azl::saveCompositionFile(dir.path / "show.json", {c});
    const auto loaded = azl::loadCompositionFile(dir.path / "show.json");
    REQUIRE(loaded);
    CHECK(loaded->composition.palettes == c.palettes);

    const nlohmann::json plain = azl::defaultComposition();
    CHECK_FALSE(plain.contains("palettes"));
    azl::saveCompositionFile(dir.path / "old.json", {azl::defaultComposition()});
    const auto old = azl::loadCompositionFile(dir.path / "old.json");
    REQUIRE(old);
    CHECK(old->composition.palettes.empty());
}

TEST_CASE("palette: nocturne-ctl adds, sets, renames, removes and lists palette colours")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    azl::WebSocketServer server(harness.document(), 0);
    const uint16_t port = server.port();
    ctl(port, "palette add green '#33e64d'");
    ctl(port, "palette add purple 80198080");
    REQUIRE(harness.document().document().palettes.size() == 1);
    CHECK(harness.document().document().palettes[0].name == "Palette 1");
    CHECK(harness.document().document().palettes[0].colours.size() == 2);
    ctl(port, "palette set green '#ff0000'");
    ctl(port, "palette name 2 violet");
    const azl::ColourPalette palette = harness.document().document().palettes[0];
    CHECK(palette.colours[0].colour == std::array<double, 4>{1, 0, 0, 1});
    CHECK(palette.colours[1].name == "violet");
    CHECK(palette.colours[1].colour[3] == doctest::Approx(0x80 / 255.0));
    const std::string list = ctl(port, "palette list");
    CHECK(list.find("1  green") != std::string::npos);
    CHECK(list.find("#ff0000") != std::string::npos);
    CHECK(list.find("#80198080") != std::string::npos);

    // A linked colour group follows `palette set`.
    Client client(harness.document());
    apply(client, azl::AddEffect{1, std::nullopt, {0, "classicStrobe", {}}});
    const uint64_t strobe = harness.document().document().layers[0].effects.back().id;
    ctl(port, "palette link green " + std::to_string(strobe) + " colorRed");
    auto param = [&](const char* name) { return azl::effectById(harness.document().document(), strobe)->params.at(name); };
    CHECK(param("colorRed") == 1.0);
    CHECK(param("colorGreen") == 0.0);
    ctl(port, "palette set 1 '#0000ff'");
    CHECK(param("colorRed") == 0.0);
    CHECK(param("colorBlue") == 1.0);
    CHECK(ctl(port, "palette list").find("linked: effect " + std::to_string(strobe) + " colorRed") != std::string::npos);
    ctl(port, "palette unlink " + std::to_string(strobe) + " colorRed");
    ctl(port, "palette set 1 '#ffffff'");
    CHECK(param("colorRed") == 0.0);

    ctl(port, "palette remove green");
    CHECK(harness.document().document().palettes[0].colours.size() == 1);
}

TEST_CASE("palette: a linked colour follows its palette colour; a palette change recolours every linked colour as one "
          "undo step, a removed palette colour unlinks, and undo brings it all back")
{
    azl::Composition c = azl::defaultComposition();
    azl::Edit palette = azl::SetPalette{flyer()};
    azl::applyEdit(c, palette);
    azl::Edit threshold = azl::AddEffect{1, std::nullopt, {0, "classicThreshold", {}}};
    azl::applyEdit(c, threshold);
    azl::Edit colorize = azl::AddEffect{1, std::nullopt, {0, "colorize", {}}};
    azl::applyEdit(c, colorize);
    azl::Edit text = azl::SetSlot{1, 1, azl::ClipRef{.name = "Text", .text = azl::TextClip{.text = "hi"}}};
    azl::applyEdit(c, text);
    const azl::ColourRef first{std::get<azl::AddEffect>(threshold).effect.id, "color1Red"};
    const azl::ColourRef second{first.effect, "color2Red"};
    const azl::ColourRef hue{std::get<azl::AddEffect>(colorize).effect.id, "hue"};
    const azl::ColourRef fill{0, azl::kTextColour, 1, 1};
    const uint64_t green = c.palettes[0].colours[0].id, purple = c.palettes[0].colours[1].id;
    auto rgba = [&](const azl::ColourRef& ref) { return azl::toRgba(*azl::colourOf(c, ref)); };
    auto link = [&](const azl::ColourRef& ref, std::optional<uint64_t> id) {
        azl::Edit edit = azl::SetPaletteLink{ref, id};
        return azl::applyEdit(c, edit);
    };

    const azl::Rgba white = rgba(first);
    azl::Edit undoLink = link(first, green);
    CHECK(azl::linkOf(c, first) == green);
    CHECK(rgba(first).g == doctest::Approx(0.9));
    azl::applyEdit(c, undoLink);
    CHECK_FALSE(azl::linkOf(c, first));
    CHECK(rgba(first) == white);
    link(first, green);
    link(second, green);
    link(hue, purple);
    link(fill, purple);
    CHECK(azl::linkedColours(c).size() == 4);
    CHECK(rgba(hue).r == doctest::Approx(0.5));
    CHECK(rgba(hue).b == doctest::Approx(0.7));
    CHECK(rgba(fill).b == doctest::Approx(0.7));

    // Recolouring green recolours both of its colours, and undo brings them back.
    azl::ColourPalette red = c.palettes[0];
    red.colours[0].colour = {1.0, 0.0, 0.0, 1.0};
    azl::Edit recolour = azl::SetPalette{red};
    azl::Edit undoRecolour = azl::applyEdit(c, recolour);
    CHECK(rgba(first) == azl::Rgba{1, 0, 0, 1});
    CHECK(rgba(second) == azl::Rgba{1, 0, 0, 1});
    CHECK(rgba(fill).b == doctest::Approx(0.7));
    azl::applyEdit(c, undoRecolour);
    CHECK(rgba(first).g == doctest::Approx(0.9));

    // Removing purple unlinks its colours, which keep it; undo links them again.
    azl::ColourPalette fewer = c.palettes[0];
    fewer.colours.erase(fewer.colours.begin() + 1);
    azl::Edit remove = azl::SetPalette{fewer};
    azl::Edit undoRemove = azl::applyEdit(c, remove);
    CHECK_FALSE(azl::linkOf(c, fill));
    CHECK_FALSE(azl::linkOf(c, hue));
    CHECK(rgba(fill).b == doctest::Approx(0.7));
    CHECK(azl::linkOf(c, first) == green);
    azl::applyEdit(c, undoRemove);
    CHECK(azl::linkOf(c, fill) == purple);
    CHECK(azl::linkOf(c, hue) == purple);

    // Removing the palette unlinks everything; undo links it again.
    azl::Edit drop = azl::RemovePalette{c.palettes[0].id};
    azl::Edit undoDrop = azl::applyEdit(c, drop);
    CHECK(azl::linkedColours(c).empty());
    azl::applyEdit(c, undoDrop);
    CHECK(azl::linkedColours(c).size() == 4);

    azl::Edit missing = azl::SetPaletteLink{first, 99};
    CHECK_THROWS_AS(azl::applyEdit(c, missing), azl::ProtocolError);
    azl::Edit slot = azl::SetPaletteLink{{.group = azl::kPaletteSlot, .palette = c.palettes[0].id}, green};
    CHECK_THROWS_AS(azl::applyEdit(c, slot), azl::ProtocolError);

    // Links travel as JSON and are saved with the show.
    const azl::Edit json = azl::editFromJson(azl::editToJson(azl::SetPaletteLink{fill, purple}));
    CHECK(std::get<azl::SetPaletteLink>(json).colour == fill);
    CHECK(std::get<azl::SetPaletteLink>(json).link == purple);
    const azl::Composition back = nlohmann::json(c).get<azl::Composition>();
    CHECK(azl::linkedColours(back) == azl::linkedColours(c));
    CHECK(nlohmann::json(azl::defaultComposition()).dump().find("links") == std::string::npos);
}
