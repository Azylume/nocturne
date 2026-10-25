#include "effects/classic_presets.hpp"
#include "effects/effect_registry.hpp"
#include "engine/chain_compiler.hpp"
#include "golden.hpp"
#include "media/composition_media.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <doctest.h>

#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <tuple>

namespace {

using azl::AddEffect;

Message addEffect(Client& client, const std::string& type, std::map<std::string, double> params = {})
{
    client.send(msg::command(AddEffect{1, std::nullopt, {0, type, std::move(params)}}));
    return client.next();
}

void write(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

// A preset file in the layout the other app saves, with made-up values.
std::string stripperPreset(const std::string& number, const std::string& mixer)
{
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<Preset name="P" className="Stripper">
  <RenderPass name="Stripper" type="DryWetEffect" baseType="DryWetEffect">
    <Params name="Params">
      <Param name="Bypassed" default="0" value="0"/>
      <ParamRange name="Opacity" default="1" value="0.75"/>
    </Params>
    <RenderPass name="Stripper" type="FFGLEffect" baseType="Effect">
      <Params name="Params">
        <Param name="Bypassed" default="0" value="0"/>
        <ParamRange name="Number" default="0.1" value=")" + number + R"("/>
        <ParamRange name="Direction" default="0" value="0.25"><PhaseSourceTimeline name="PhaseSourceTimeline"/></ParamRange>
        <ParamRange name="Direction Spread" default="0" value="0.5"/>
        <ParamRange name="GlobalWidth" default="1" value="1.7"/>
        <ParamRange name="Wobble" default="0" value="0.5"/>
      </Params>
    </RenderPass>
    <ChoosableMixer name="Mixer"><RenderPass name=")" + mixer + R"(" type=")" + mixer + R"(" baseType="Mixer"/></ChoosableMixer>
  </RenderPass>
</Preset>
)";
}

const char* kColorizePreset = R"(<?xml version="1.0" encoding="UTF-8"?>
<Preset name="P" className="Colorize">
  <RenderPass name="Colorize" type="DryWetEffect" baseType="DryWetEffect">
    <RenderPass name="Colorize" type="Colorize" baseType="Effect">
      <Params name="Params">
        <ParamColor name="Color" T="COLOR" default="4278190335" value="4294902015">
          <Params name="Channels">
            <ParamRange name="Red" T="DOUBLE" default="1" value="0.5"/>
            <ParamRange name="Green" T="DOUBLE" default="0" value="0.25"/>
            <ParamRange name="Blue" T="DOUBLE" default="0" value="1"/>
            <ParamRange name="Hue" T="DOUBLE" default="0" value="0.7"/>
            <ParamRange name="Alpha" T="DOUBLE" default="1" value="0.5"/>
          </Params>
        </ParamColor>
        <ParamChoice name="Mode" T="INT32" default="0" value="1" storeChoices="0"/>
        <Param name="GreysKeep" altName="Keep" T="BOOL" default="0" value="1"/>
        <ParamRange name="Contrast" T="DOUBLE" default="0.2" value="0.6"/>
      </Params>
    </RenderPass>
    <ChoosableMixer name="Mixer"><RenderPass name="Alpha" type="Alpha" baseType="Mixer"/></ChoosableMixer>
  </RenderPass>
</Preset>
)";

// The probe inputs the parity harness writes, drawn as 8-bit pictures: R = x, G = y, B = 128, and the flat colour
// 200/100/50 with alpha from 64 at the top to 255 at the bottom.
const std::map<std::string, std::pair<std::string, std::string>> kProbes{
    {"uv.png",
     {"parityUv", "\n    float2 at = round(floor(px) * 255.0 / (h.size - 1.0));\n"
                  "    return float4(at / 255.0, 128.0 / 255.0, 1.0);\n"}},
    {"alpha.png",
     {"parityAlpha", "\n    float a = round(64.0 + floor(px.y) * 191.0 / (h.size.y - 1.0));\n"
                     "    return float4(200.0, 100.0, 50.0, a) / 255.0;\n"}}};

// Registers a probe input as a pointwise effect, once, and returns its type.
std::string probeEffect(const std::string& input)
{
    const auto& [type, metal] = kProbes.at(input);
    if (!azl::findEffect(type))
        azl::registerEffect({.type = type, .kind = azl::StageKind::Pointwise, .metal = metal});
    return type;
}

} // namespace

TEST_CASE("Classic effects expose exactly the inventoried controls, in order, with their defaults and stored ranges")
{
    const fs::path spec = sourceDir() / "docs/research/classic-controls.json";
    if (!fs::exists(spec)) {
        MESSAGE("no control spec at " << spec << "; skipped");
        return;
    }
    std::ifstream in(spec);
    const nlohmann::json effects = nlohmann::json::parse(in)["effects"];
    for (const auto& effect : effects) {
        const azl::EffectDef* def = azl::findEffect(effect["classicType"].get<std::string>());
        if (!def)
            continue;
        CAPTURE(def->type);
        CHECK(def->category == "Classic");
        std::vector<std::string> expected;
        for (const auto& param : effect["params"]) {
            const std::string id = param["id"];
            if (param["kind"] == "color" || id.ends_with("Hue") || id.ends_with("Saturation") ||
                id.ends_with("Brightness"))
                continue;
            expected.push_back(id);
            auto p = std::find_if(def->params.begin(), def->params.end(), [&](const azl::ParamDef& d) { return d.name == id; });
            REQUIRE(p != def->params.end());
            CHECK(p->defaultValue == doctest::Approx(param["default"].get<double>()));
            if (param.contains("range")) {
                CHECK(p->min == param["range"][0].get<double>());
                CHECK(p->max == param["range"][1].get<double>());
            }
            if (!param["label"].get<std::string>().empty() && id.find("color") != 0)
                CHECK((p->fullLabel.empty() ? p->label : p->fullLabel) == param["label"].get<std::string>());
            if (param.contains("options"))
                CHECK(p->options == param["options"].get<std::vector<std::string>>());
            if (param["kind"] == "bool")
                CHECK(p->options == std::vector<std::string>{"Off", "On"});
        }
        std::vector<std::string> actual;
        for (const azl::ParamDef& p : def->params)
            if (p.name != "mix" && p.name != "blendMode" && p.name != "syncToBeat")
                actual.push_back(p.name);
        CHECK(actual == expected);
        CHECK(def->params.back().label == "Opacity");
    }
    CHECK(azl::findEffect("classicStripper"));
    CHECK(azl::findEffect("classicColorize"));
}

TEST_CASE("preset files map onto Classic effects by name, keep their names, and an earlier folder wins")
{
    TempDir user, bundled;
    write(user.path / "Video Effects/Stripper/Wide.xml", stripperPreset("0.3", "Add"));
    write(user.path / "Video Effects/Colorize/Violet.xml", kColorizePreset);
    write(user.path / "Video Effects/Colorize/Broken.xml", "<Preset");
    write(bundled.path / "Video Effects/Stripper/Wide.xml", stripperPreset("0.9", "Alpha"));
    write(bundled.path / "Video Effects/Stripper/Plain.xml", stripperPreset("0.2", "Multiply"));
    write(bundled.path / "Video Effects/Blur/Basic.xml",
          R"(<Preset><RenderPass type="DryWetEffect"><RenderPass name="Blur" type="Blur"/></RenderPass></Preset>)");

    const std::vector<azl::ClassicPreset> presets = azl::readClassicPresets({user.path, bundled.path});
    std::map<std::string, azl::ClassicPreset> byName;
    for (const azl::ClassicPreset& p : presets)
        byName[p.name] = p;
    REQUIRE(byName.size() == 3);

    const azl::ClassicPreset& wide = byName["Wide"];
    CHECK(wide.effectType == "classicStripper");
    CHECK(wide.params.at("number") == doctest::Approx(0.3));
    CHECK(wide.params.at("direction") == doctest::Approx(0.25));
    CHECK(wide.params.at("directionSpread") == doctest::Approx(0.5));
    CHECK(wide.params.at("globalWidth") == 1.0);
    CHECK(wide.params.at("mix") == doctest::Approx(0.75));
    CHECK(wide.params.at("blendMode") == 1.0);
    CHECK(wide.skipped == std::vector<std::string>{"Direction animation", "Wobble"});
    CHECK(byName["Plain"].skipped.back() == "Multiply mixer");
    CHECK(!byName["Plain"].params.contains("blendMode"));

    const azl::ClassicPreset& violet = byName["Violet"];
    CHECK(violet.effectType == "classicColorize");
    CHECK(violet.params == std::map<std::string, double>{{"colorRed", 0.5},
                                                         {"colorGreen", 0.25},
                                                         {"colorBlue", 1.0},
                                                         {"colorAlpha", 0.5},
                                                         {"mode", 1.0},
                                                         {"greysKeep", 1.0},
                                                         {"contrast", 0.6}});
    CHECK(violet.skipped.empty());
}

TEST_CASE("imported Classic presets list and apply like saved ones and never reach the presets file")
{
    TempDir root, presets;
    write(presets.path / "Video Effects/Stripper/Wide.xml", stripperPreset("0.3", "Add"));
    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, root.path);
    Client client(harness.document());
    const uint64_t effect = addEffect(client, "classicStripper")["effect"]["id"];

    client.send({{"type", "preset"}, {"op", "import"}, {"roots", {presets.path.string()}}});
    const Message imported = client.next();
    CHECK(imported["presets"]["classicStripper"]["Wide"]["number"] == 0.3);
    CHECK(imported["skipped"]["classicStripper"]["Wide"][1] == "Wobble");
    client.send({{"type", "preset"}, {"op", "list"}});
    CHECK(client.next()["presets"]["classicStripper"].contains("Wide"));
    client.send({{"type", "preset"}, {"op", "apply"}, {"layer", 1}, {"effect", effect}, {"name", "Wide"}});
    const Message applied = client.next();
    CHECK(applied["op"] == "setEffectParams");
    CHECK(applied["params"]["blendMode"] == 1.0);
    CHECK(!fs::exists(root.path / "presets.json"));
}

TEST_CASE("the preset list names each preset's group; rename and delete act on saved presets only")
{
    TempDir root, presets;
    write(presets.path / "Video Effects/Stripper/Wide.xml", stripperPreset("0.3", "Add"));
    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, root.path);
    Client client(harness.document());
    const uint64_t effect = addEffect(client, "classicStripper", {{"number", 0.7}})["effect"]["id"];
    client.send({{"type", "preset"}, {"op", "import"}, {"roots", {presets.path.string()}}});
    client.next();
    client.send({{"type", "preset"}, {"op", "save"}, {"effect", effect}, {"name", "Wide"}});
    client.next();

    auto groupsOf = [&](const std::string& name) {
        client.send({{"type", "preset"}, {"op", "list"}});
        const Message list = client.next();
        std::vector<std::string> groups;
        for (const auto& entry : list["entries"])
            if (entry["effectType"] == "classicStripper" && entry["name"] == name)
                groups.push_back(entry["group"]);
        return std::make_pair(groups, list);
    };
    const auto [wide, list] = groupsOf("Wide");
    CHECK(wide == std::vector<std::string>{"imported", "mine"});
    CHECK(list["presets"]["classicStripper"]["Wide"]["number"] == 0.7);
    CHECK(groupsOf("Two-Way Bars").first == std::vector<std::string>{"builtin"});

    client.send({{"type", "preset"}, {"op", "apply"}, {"effect", effect}, {"name", "Wide"}, {"group", "imported"}});
    CHECK(client.next()["params"]["number"] == 0.3);

    client.send({{"type", "preset"}, {"op", "rename"}, {"effectType", "classicStripper"}, {"name", "Two-Way Bars"},
                 {"to", "Mine now"}});
    CHECK(client.next()["type"] == "error");
    client.send({{"type", "preset"}, {"op", "rename"}, {"effectType", "classicStripper"}, {"name", "Wide"}, {"to", "Seven"}});
    CHECK(client.next()["to"] == "Seven");
    CHECK(groupsOf("Wide").first == std::vector<std::string>{"imported"});
    CHECK(groupsOf("Seven").first == std::vector<std::string>{"mine"});

    client.send({{"type", "preset"}, {"op", "delete"}, {"effectType", "classicStripper"}, {"name", "Wide"}});
    CHECK(client.next()["type"] == "error");
    client.send({{"type", "preset"}, {"op", "delete"}, {"effectType", "classicStripper"}, {"name", "Seven"}});
    CHECK(client.next()["op"] == "delete");
    CHECK(groupsOf("Seven").first.empty());
    std::ifstream file(root.path / "presets.json");
    CHECK(nlohmann::json::parse(file)["presets"].empty());
}

TEST_CASE("Classic Colorize tints greys by their level and its Add mixer crossfades additively, as measured")
{
    auto grey = [](std::map<std::string, double> params) {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "classicColorize", std::move(params));
        harness.step();
        const azl::Image image = harness.readScreen();
        const size_t at = (90 * 960 + 165) * 4;
        return std::vector<int>{image.rgba[at], image.rgba[at + 1], image.rgba[at + 2]};
    };
    CHECK(grey({{"colorAlpha", 0.0}}) == std::vector<int>{191, 191, 191});
    CHECK(grey({}) == std::vector<int>{191, 0, 0});
    CHECK(grey({{"mix", 0.5}}) == std::vector<int>{191, 96, 96});
    CHECK(grey({{"mix", 0.5}, {"blendMode", 1.0}}) == std::vector<int>{255, 191, 191});
}

TEST_CASE("Classic colour ops fuse into one pass, are neutral at their defaults, follow their formulas, and each edit "
          "is one undo step")
{
    azl::EngineHarness plain({.width = 960, .height = 540});
    plain.step();
    const azl::Image card = plain.readScreen();
    // The first two colour bars of the test card: grey 0.75 and yellow (0.75, 0.75, 0).
    auto bars = [](const azl::Image& image) {
        std::vector<int> out;
        for (const size_t x : {165, 255})
            for (int c = 0; c < 3; ++c)
                out.push_back(image.rgba[(90 * 960 + x) * 4 + c]);
        return out;
    };
    auto render = [](const std::string& type, std::map<std::string, double> params) {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, type, std::move(params));
        harness.step();
        return harness.readScreen();
    };
    auto near = [](const std::vector<int>& a, const std::vector<int>& b) {
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs(a[i] - b[i]) > 1)
                return false;
        return true;
    };
    std::vector<const azl::EffectDef*> chain{azl::findEffect("classicColorize")};
    for (const char* type : {"classicBrightnessContrast", "classicLevels", "classicThreshold", "classicHueRotate"})
        chain.push_back(azl::findEffect(type));
    CHECK(azl::planChain(azl::SourceKind::Yuv, chain).size() == 1);
    for (const char* type : {"classicBrightnessContrast", "classicLevels", "classicHueRotate"}) {
        CAPTURE(type);
        CHECK(near(bars(render(type, {})), bars(card)));
    }
    CHECK(near(bars(render("classicBrightnessContrast", {{"brightness", -0.25}})), {128, 128, 128, 128, 128, 0}));
    CHECK(near(bars(render("classicBrightnessContrast", {{"contrast", 0.04}})), {230, 230, 230, 230, 230, 0}));
    CHECK(near(bars(render("classicLevels", {{"base", 0.55}})), {242, 242, 242, 242, 242, 51}));
    CHECK(near(bars(render("classicLevels", {{"ratio", 0.25}})), {48, 48, 48, 48, 48, 0}));
    CHECK(near(bars(render("classicThreshold", {})), {255, 255, 255, 255, 255, 255}));
    CHECK(near(bars(render("classicThreshold", {{"threshold", 0.7}})), {255, 255, 255, 0, 0, 0}));
    CHECK(near(bars(render("classicThreshold", {{"threshold", 0.7}, {"color2Blue", 1.0}})),
               {255, 255, 255, 0, 0, 255}));
    CHECK(near(bars(render("classicHueRotate", {{"hueRotate", 0.5}})), {191, 191, 191, 0, 0, 191}));
    CHECK(near(bars(render("classicHueRotate", {{"satScale", 0.0}})), {191, 191, 191, 96, 96, 96}));

    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    for (const auto& [type, param, value, before] : {std::tuple{"classicBrightnessContrast", "contrast", 0.4, 0.0},
                                                     {"classicLevels", "midpoint", 0.3, 0.5},
                                                     {"classicThreshold", "color1Green", 0.2, 1.0},
                                                     {"classicHueRotate", "hueRotate", 0.25, 0.0}}) {
        CAPTURE(type);
        const uint64_t effect = addEffect(client, type)["effect"]["id"];
        client.send(msg::command(azl::SetEffectParam{effect, param, value}));
        CHECK(client.next()["value"] == doctest::Approx(value));
        client.send(msg::undo());
        client.next();
        CHECK(harness.document().document().layers[0].effects.back().params.at(param) == before);
    }
}

TEST_CASE("Classic Stripper compiles its resampling modes 3 and 4 as a neighbourhood variant, the others as before")
{
    azl::Composition composition;
    composition.layers.push_back({.id = 1, .name = "Layer 1", .effects = {{1, "classicStripper", {}}}});
    azl::Effect& stripper = composition.layers[0].effects[0];
    for (int mode = 0; mode <= 6; ++mode) {
        CAPTURE(mode);
        stripper.params["mode"] = mode / 6.0;
        const bool resamples = mode == 3 || mode == 4;
        CHECK(azl::shaderType(composition, stripper) == (resamples ? "classicStripper_resample" : "classicStripper"));
    }
    CHECK(azl::findEffect("classicStripper_resample")->kind == azl::StageKind::Neighbourhood);
    CHECK(azl::findEffect("classicStripper")->kind == azl::StageKind::Pointwise);
}

TEST_CASE("Classic Stripper's per-pixel strip loop hashes nothing and takes no cosine or sine for its strips")
{
    // The plain variant's strips are its mask, which the stage takes first.
    for (const char* type : {"classicStripper", "classicStripper_resample"}) {
        CAPTURE(type);
        const azl::EffectDef& effect = *azl::findEffect(type);
        const std::string& metal = effect.mask.empty() ? effect.metal : effect.mask;
        const size_t loop = metal.find("for (int i = 0; i < count; ++i)");
        REQUIRE(loop != std::string::npos);
        const std::string body = metal.substr(loop);
        for (const char* perStrip : {"cos(", "sin(", "pcg(n)", "pcg(uint(i)"})
            CHECK_MESSAGE(body.find(perStrip) == std::string::npos, perStrip);
        // The edge's smoothstep runs only near the edge.
        CHECK(body.find("? 1.0") < body.find("smoothstep("));
        CHECK(body.find("? 0.0") < body.find("smoothstep("));
    }
    // Only the plain variant stops early: the resampling one adds every strip it is in.
    CHECK(azl::findEffect("classicStripper")->mask.find("break;") != std::string::npos);
    CHECK(azl::findEffect("classicStripper_resample")->metal.find("break;") == std::string::npos);
}

TEST_CASE("Classic Mirror Quad and Polar Kaleido read the picture where measured, map their presets, and each edit is "
          "one undo step")
{
    constexpr double kW = 960.0, kH = 540.0, kPi = 3.14159265358979;
    auto frac = [](double v) { return v - std::floor(v); };
    using Mapping = std::function<std::pair<double, double>(double, double)>;
    // Reads the source position back from the UV probe at points across the picture, away from wrap seams.
    auto check = [&](const std::string& type, std::map<std::string, double> params, const Mapping& expected) {
        CAPTURE(type);
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, probeEffect("uv.png"));
        addEffect(client, type, std::move(params));
        harness.step();
        const azl::Image image = harness.readScreen();
        int checked = 0;
        for (double y = 7.5; y < kH; y += 37.0)
            for (double x = 5.5; x < kW; x += 41.0) {
                const auto [u, v] = expected(x, y);
                if (std::min({u, 1.0 - u, v, 1.0 - v}) < 0.02)
                    continue;
                const size_t at = (size_t(y) * 960 + size_t(x)) * 4;
                CAPTURE(x);
                CAPTURE(y);
                CHECK(std::abs(image.rgba[at] - u * 255.0) <= 2.5);
                CHECK(std::abs(image.rgba[at + 1] - v * 255.0) <= 2.5);
                ++checked;
            }
        CHECK(checked > 250);
    };
    auto quad = [&](bool flipX, bool flipY) {
        return [=](double x, double y) {
            const double fx = std::abs(2.0 * x / kW - 1.0), fy = std::abs(2.0 * y / kH - 1.0);
            return std::pair{flipX ? fx : 1.0 - fx, flipY ? fy : 1.0 - fy};
        };
    };
    check("classicMirrorQuad", {}, quad(false, false));
    check("classicMirrorQuad", {{"flipX", 1.0}}, quad(true, false));
    check("classicMirrorQuad", {{"flipX", 1.0}, {"flipY", 1.0}}, quad(true, true));
    auto polar = [&](double rings, double parts, double angle) {
        return [=](double x, double y) {
            const double dx = x - kW / 2, dy = kH / 2 - y;
            return std::pair{frac(parts * (std::atan2(dy, dx) / (2 * kPi) + 0.75) - angle),
                             frac(std::hypot(dx, dy) / (kW / 2) * (rings + 1) / 2)};
        };
    };
    check("classicPolarKaleido", {}, polar(1, 1, 0));
    check("classicPolarKaleido", {{"rings", 3.0}, {"parts", 5.0}, {"angle", 0.125}}, polar(3, 5, 0.125));
    check("classicPolarKaleido", {{"rings", 2.4}, {"parts", 3.1}}, polar(2, 3, 0));

    TempDir presets;
    write(presets.path / "Video Effects/MirrorQuad/Flipped.xml", R"(<Preset><RenderPass type="DryWetEffect">
  <RenderPass name="MirrorQuad" type="MirrorQuad"><Params name="Params">
    <Param name="Flip X" T="BOOL" default="0" value="1"/><Param name="Flip Y" T="BOOL" default="0" value="0"/>
  </Params></RenderPass></RenderPass></Preset>)");
    write(presets.path / "Video Effects/PolarKaleido/Spun.xml", R"(<Preset><RenderPass type="DryWetEffect">
  <RenderPass name="PolarKaleido" type="PolarKaleido"><Params name="Params">
    <ParamRange name="Rings" default="1" value="4"/><ParamRange name="Parts" default="1" value="3.1026"/>
    <ParamRange name="Angle" default="0" value="0.3"/>
  </Params></RenderPass></RenderPass></Preset>)");
    std::map<std::string, azl::ClassicPreset> byName;
    for (const azl::ClassicPreset& p : azl::readClassicPresets({presets.path}))
        byName[p.name] = p;
    CHECK(byName["Flipped"].effectType == "classicMirrorQuad");
    CHECK(byName["Flipped"].params == std::map<std::string, double>{{"flipX", 1.0}, {"flipY", 0.0}});
    CHECK(byName["Spun"].effectType == "classicPolarKaleido");
    CHECK(byName["Spun"].params ==
          std::map<std::string, double>{{"rings", 4.0}, {"parts", 3.1026}, {"angle", 0.3}});
    CHECK(byName["Spun"].skipped.empty());

    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    for (const auto& [type, param, value, before] : {std::tuple{"classicMirrorQuad", "flipY", 1.0, 0.0},
                                                     {"classicPolarKaleido", "rings", 6.0, 1.0},
                                                     {"classicPolarKaleido", "angle", 0.4, 0.0}}) {
        CAPTURE(param);
        const uint64_t effect = addEffect(client, type)["effect"]["id"];
        client.send(msg::command(azl::SetEffectParam{effect, param, value}));
        CHECK(client.next()["value"] == doctest::Approx(value));
        client.send(msg::undo());
        client.next();
        CHECK(harness.document().document().layers[0].effects.back().params.at(param) == before);
    }
}

TEST_CASE("Classic Edge Detection behaves as measured, maps its presets, and each edit is one undo step")
{
    auto render = [](const std::string& type, std::map<std::string, double> params) {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, type, std::move(params));
        harness.step();
        return harness.readScreen();
    };
    auto lit = [](const azl::Image& image, int channel, int above) {
        size_t n = 0;
        for (size_t i = channel; i < image.rgba.size(); i += 4)
            n += image.rgba[i] > above;
        return n;
    };

    // Edges: none at multiplier 0, flat areas black, a hard edge saturates; mode 0 lights one side of an edge only;
    // Use Color draws the colour.
    CHECK(lit(render("classicEdgeDetection", {{"multiplier", 0.0}}), 0, 0) == 0);
    const azl::Image edges = render("classicEdgeDetection", {});
    CHECK(edges.rgba[(90 * 960 + 165) * 4] == 0);
    CHECK(lit(edges, 0, 254) > 1000);
    CHECK(lit(render("classicEdgeDetection", {{"mode", 0.0}}), 0, 254) < lit(edges, 0, 254) * 3 / 4);
    const azl::Image red = render("classicEdgeDetection", {{"useColor", 1.0}, {"colorGreen", 0.0}, {"colorBlue", 0.0}});
    CHECK(lit(red, 0, 254) > 1000);
    CHECK(lit(red, 1, 0) == 0);
    CHECK(lit(red, 2, 0) == 0);
    CHECK(lit(render("classicEdgeDetection", {{"amount", 0.0}}), 0, 254) == lit(edges, 0, 254));

    TempDir presets;
    write(presets.path / "Video Effects/EdgeDetection/Red.xml", R"(<Preset><RenderPass type="DryWetEffect">
  <RenderPass name="EdgeDetection" type="EdgeDetection"><Params name="Params">
    <ParamRange name="Mode" default="1" value="0"/><ParamRange name="Multiplier" default="0.2" value="0.5"/>
    <Param name="Use Color" T="BOOL" default="0" value="1"/>
    <ParamColor name="Color" T="COLOR" default="4294967295" value="4278190335"><Params name="Channels">
      <ParamRange name="Red" T="DOUBLE" default="1" value="1"/><ParamRange name="Green" T="DOUBLE" default="1" value="0"/>
      <ParamRange name="Blue" T="DOUBLE" default="1" value="0"/><ParamRange name="Hue" T="DOUBLE" default="0" value="0"/>
    </Params></ParamColor>
  </Params></RenderPass></RenderPass></Preset>)");
    std::map<std::string, azl::ClassicPreset> byName;
    for (const azl::ClassicPreset& p : azl::readClassicPresets({presets.path}))
        byName[p.name] = p;
    CHECK(byName["Red"].effectType == "classicEdgeDetection");
    CHECK(byName["Red"].params == std::map<std::string, double>{{"mode", 0.0}, {"multiplier", 0.5}, {"useColor", 1.0},
                                                                {"colorRed", 1.0}, {"colorGreen", 0.0},
                                                                {"colorBlue", 0.0}});
    CHECK(byName["Red"].skipped.empty());

    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    for (const auto& [type, param, value, before] : {std::tuple{"classicEdgeDetection", "multiplier", 0.6, 0.2},
                                                     {"classicEdgeDetection", "useColor", 1.0, 0.0}}) {
        CAPTURE(param);
        const uint64_t effect = addEffect(client, type)["effect"]["id"];
        client.send(msg::command(azl::SetEffectParam{effect, param, value}));
        CHECK(client.next()["value"] == doctest::Approx(value));
        client.send(msg::undo());
        client.next();
        CHECK(harness.document().document().layers[0].effects.back().params.at(param) == doctest::Approx(before));
    }
}

TEST_CASE("built-in Classic looks set every parameter in range, have unique names, and are served with the presets")
{
    std::map<std::string, size_t> looks;
    std::set<std::string> names;
    for (const azl::ClassicPreset& look : azl::classicBuiltinPresets()) {
        CAPTURE(look.name);
        const azl::EffectDef* def = azl::findEffect(look.effectType);
        REQUIRE(def);
        CHECK(look.params.size() == def->params.size());
        for (const azl::ParamDef& p : def->params) {
            REQUIRE(look.params.contains(p.name));
            CHECK(look.params.at(p.name) >= p.min);
            CHECK(look.params.at(p.name) <= p.max);
        }
        ++looks[look.effectType];
        CHECK(names.insert(look.name).second);
    }
    TempDir root;
    azl::EngineHarness harness({.width = 960, .height = 540}, 60.0, root.path);
    Client client(harness.document());
    const uint64_t effect = addEffect(client, "classicKaleidoscope")["effect"]["id"];
    client.send({{"type", "preset"}, {"op", "list"}});
    CHECK(client.next()["presets"]["classicMirror"].size() == looks["classicMirror"]);
    client.send({{"type", "preset"}, {"op", "apply"}, {"effect", effect}, {"name", "Nine Petals"}});
    CHECK(client.next()["params"]["angles"] == doctest::Approx(0.81));
    CHECK(!fs::exists(root.path / "presets.json"));
}

TEST_CASE("built-in presets: each of Nocturne's own effects has 3 to 5 looks with every parameter in range, listed by "
          "nocturne-ctl preset list and applied by preset apply")
{
    const std::vector<std::string> own = {"transform", "colorize", "edgeDetection", "stripper", "trails", "reveal"};
    std::map<std::string, std::set<std::string>> names;
    for (const azl::ClassicPreset& look : azl::builtinPresets()) {
        CAPTURE(look.name);
        const azl::EffectDef* def = azl::findEffect(look.effectType);
        REQUIRE(def);
        CHECK(look.params.size() == def->params.size());
        for (const azl::ParamDef& p : def->params) {
            REQUIRE(look.params.contains(p.name));
            CHECK(look.params.at(p.name) >= p.min);
            CHECK(look.params.at(p.name) <= p.max);
        }
        CHECK(names[look.effectType].insert(look.name).second);
    }
    for (const std::string& type : own) {
        CAPTURE(type);
        CHECK(names[type].size() >= 3);
        CHECK(names[type].size() <= 5);
    }
    CHECK(names["classicStripper"].size() == 6);

    TempDir root;
    azl::EngineHarness harness({.width = 320, .height = 180}, 60.0, root.path);
    azl::WebSocketServer server(harness.document(), 0);
    const auto ctl = [&](const std::string& args) {
        const std::string command =
            "'" + ctlPath().string() + "' --port " + std::to_string(server.port()) + " " + args + " 2>&1";
        std::string out;
        FILE* pipe = popen(command.c_str(), "r");
        REQUIRE(pipe);
        char buffer[512];
        while (fgets(buffer, sizeof buffer, pipe))
            out += buffer;
        CHECK(pclose(pipe) == 0);
        return out;
    };
    for (const std::string& type : own) {
        CAPTURE(type);
        const std::string listed = ctl("preset list " + type);
        for (const std::string& name : names[type])
            CHECK_MESSAGE(listed.find(name) != std::string::npos, name);
    }
    ctl("fx " + std::to_string(harness.document().document().layers[0].id) + " reveal");
    REQUIRE(harness.document().document().layers[0].effects.size() == 1);
    const uint64_t reveal = harness.document().document().layers[0].effects[0].id;
    ctl("preset apply " + std::to_string(reveal) + " 'Big Blocks'");
    CHECK(harness.document().document().layers[0].effects[0].params.at("columns") == 4.0);
    CHECK(harness.document().document().layers[0].effects[0].params.at("rows") == 3.0);
    CHECK(!fs::exists(root.path / "presets.json"));
}

TEST_CASE("a Classic look renders the same with each option parameter set to the value its option stores")
{
    std::vector<azl::ClassicPreset> looks = azl::classicBuiltinPresets();
    looks.push_back({"classicStripper", "golden values", {{"number", 0.5}, {"mode", 0.34}, {"movementType", 0.3},
                                                         {"blendMode", 1.0}, {"mix", 0.5}}});
    auto render = [](const std::string& type, const std::map<std::string, double>& params) {
        azl::EngineHarness harness({.width = 480, .height = 270});
        Client client(harness.document());
        addEffect(client, type, params);
        harness.step(3);
        return harness.readScreen();
    };
    size_t compared = 0;
    for (const azl::ClassicPreset& look : looks) {
        std::map<std::string, double> chosen = look.params;
        for (const azl::ParamDef& p : azl::findEffect(look.effectType)->params)
            if (!p.options.empty() && chosen.contains(p.name))
                chosen[p.name] = azl::optionValue(p, azl::optionIndex(p, chosen[p.name]));
        if (chosen == look.params)
            continue;
        CAPTURE(look.name);
        CHECK(render(look.effectType, look.params).rgba == render(look.effectType, chosen).rgba);
        ++compared;
    }
    CHECK(compared >= 5);
}

TEST_CASE("Classic Trails, Strobe and Shift Glitch keep their measured history, timing and bands, map their presets, "
          "and each edit is one undo step")
{
    // Trails: the test card moves half its width to the right and leaves the history of its white left edge at x = 0;
    // at (481, 200) the edge arrives over the card's dark ground.
    auto trails = [](std::map<std::string, double> params) {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        const uint64_t transform = addEffect(client, "transform")["effect"]["id"];
        addEffect(client, "classicTrails", std::move(params));
        harness.step();
        client.send(msg::command(azl::SetEffectParam{transform, "positionX", 0.5}));
        client.next();
        std::vector<int> left, arrived;
        for (int i = 0; i < 3; ++i) {
            harness.step();
            const azl::Image image = harness.readScreen();
            left.push_back(pixel(image, 0, 270)[0]);
            arrived.push_back(pixel(image, 481, 200)[0]);
        }
        return std::pair{left, arrived};
    };
    auto near = [](const std::vector<int>& a, const std::vector<int>& b) {
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs(a[i] - b[i]) > 2)
                return false;
        return true;
    };
    CHECK(near(trails({{"feedback", 0.25}}).first, {128, 64, 32}));
    CHECK(trails({{"feedback", 0.25}}).second[0] < 200);
    CHECK(near(trails({{"feedback", 0.25}, {"mode", 2.0}}).first, {151, 89, 53}));
    CHECK(near(trails({{"feedback", 0.25}, {"mode", 2.0}}).second, {255, 255, 255}));
    CHECK(near(trails({{"feedback", 0.5}, {"mode", 1.0}}).first, {255, 255, 255}));
    CHECK(near(trails({{"feedback", 0.0}, {"mode", 1.0}}).first, {0, 0, 0}));
    CHECK(near(trails({{"feedback", 1.0}}).second, {20, 20, 20}));
    CHECK(near(trails({{"feedback", 1.0}}).first, {255, 255, 255}));
    CHECK(near(trails({{"feedback", 0.0}}).first, {0, 0, 0}));
    {
        // On a playing clip, feedback 1 holds the picture the effect started on.
        TempDir media;
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        playMedia(client, copyMedia("count_240.mp4", media.path / "count.mp4"));
        harness.step(3);
        addEffect(client, "classicTrails", {{"feedback", 1.0}});
        harness.step();
        const azl::Image held = harness.readScreen();
        harness.step(20);
        CHECK(harness.readScreen().rgba == held.rgba);
    }

    // Strobe: a period of 2 x (1 - frequency) seconds, on for half of it.
    auto strobe = [](std::map<std::string, double> params, int frames) {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "classicStrobe", std::move(params));
        std::vector<int> red;
        for (int i = 0; i < frames; ++i) {
            harness.step();
            red.push_back(pixel(harness.readScreen(), 600, 300)[0]);
        }
        return red;
    };
    const std::map<std::string, double> orange{{"colorGreen", 0.5}, {"colorBlue", 0.0}};
    auto flashes = [](const std::vector<int>& red) { return std::count(red.begin(), red.end(), 255); };
    CHECK(flashes(strobe({{"frequency", 1.0}}, 10)) == 10);
    CHECK(flashes(strobe({{"frequency", 0.0}}, 120)) == 0);
    CHECK(std::abs(flashes(strobe({{"frequency", 0.5}}, 60)) - 30) <= 1);
    CHECK(std::abs(flashes(strobe({{"frequency", 0.75}}, 60)) - 30) <= 1);
    // At 90 BPM: every half second without Sync to Beat, every beat (2/3 s) with it.
    auto risesAt90 = [](double sync) {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        apply(client, azl::SetTempo{90.0});
        addEffect(client, "classicStrobe", {{"frequency", 0.75}, {"syncToBeat", sync}});
        int rises = 0;
        bool was = false;
        for (int i = 0; i < 240; ++i) {
            harness.step();
            const bool on = pixel(harness.readScreen(), 600, 300)[0] == 255;
            rises += on && !was;
            was = on;
        }
        return rises;
    };
    CHECK(std::abs(risesAt90(0.0) - 8) <= 1);
    CHECK(std::abs(risesAt90(1.0) - 6) <= 1);
    const std::vector<int> faded = strobe({{"frequency", 0.5}, {"fade", 1.0}}, 60);
    CHECK(*std::max_element(faded.begin(), faded.end()) >= 250);
    CHECK(std::count_if(faded.begin(), faded.end(), [](int v) { return v > 60 && v < 200; }) > 20);
    CHECK(strobe({{"frequency", 1.0}, {"colorAlpha", 0.5}}, 1)[0] == doctest::Approx(128).epsilon(0.02));
    {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, "classicStrobe", {{"frequency", 1.0}, {"colorGreen", 0.5}, {"colorBlue", 0.0}});
        harness.step();
        const uint8_t* p = pixel(harness.readScreen(), 600, 300);
        CHECK(std::vector<int>{p[0], p[1], p[2]} == std::vector<int>{255, 128, 0});
    }

    // Shift Glitch on the UV probe: rows read from elsewhere keep their x, columns keep their y.
    struct Moved {
        double rows = 0.0, columns = 0.0;
        bool samePattern = false;
    };
    auto glitch = [](std::map<std::string, double> params, int frames = 31, double bpm = 120.0) {
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        apply(client, azl::SetTempo{bpm});
        addEffect(client, probeEffect("uv.png"));
        addEffect(client, "classicShiftGlitch", std::move(params));
        harness.step(frames - 1);
        const azl::Image before = harness.readScreen();
        harness.step();
        const azl::Image image = harness.readScreen();
        Moved moved{0.0, 0.0, before.rgba == image.rgba};
        for (uint32_t y = 0; y < 540; ++y)
            moved.rows += std::abs(pixel(image, 7, y)[1] - std::round(y * 255.0 / 539.0)) > 1.5;
        for (uint32_t x = 0; x < 960; ++x)
            moved.columns += std::abs(pixel(image, x, 5)[0] - std::round(x * 255.0 / 959.0)) > 1.5;
        moved.rows /= 540.0;
        moved.columns /= 960.0;
        return moved;
    };
    const Moved plain = glitch({{"horizontal", 0.0}});
    CHECK(plain.rows == 0.0);
    CHECK(plain.columns == 0.0);
    CHECK(glitch({{"size", 0.0}}).rows == 0.0);
    const Moved rows = glitch({});
    CHECK(rows.rows > 0.2);
    CHECK(rows.columns == 0.0);
    CHECK(!rows.samePattern);
    const Moved columns = glitch({{"vertical", 1.0}, {"horizontal", 0.0}});
    CHECK(columns.rows == 0.0);
    CHECK(columns.columns > 0.4);
    // At frequency 0.5 the pattern lasts a beat with Sync to Beat: frames 29 and 30 share one, 30 and 31 (beat 1) do
    // not; at 90 BPM the beat ends at frame 40. Without it, half a second at any tempo.
    CHECK(glitch({{"frequency", 0.5}}, 30).samePattern);
    CHECK(!glitch({{"frequency", 0.5}}, 31).samePattern);
    CHECK(glitch({{"frequency", 0.5}}, 40, 90.0).samePattern);
    CHECK(!glitch({{"frequency", 0.5}}, 41, 90.0).samePattern);
    for (const double bpm : {120.0, 90.0}) {
        CHECK(glitch({{"frequency", 0.5}, {"syncToBeat", 0.0}}, 30, bpm).samePattern);
        CHECK(!glitch({{"frequency", 0.5}, {"syncToBeat", 0.0}}, 31, bpm).samePattern);
    }
    // A show saved before Sync to Beat existed loads with it on, so it keeps its beat timing.
    {
        azl::Composition show;
        show.layers.push_back({.id = 1, .effects = {{.id = 1, .type = "classicStrobe", .params = {{"frequency", 0.8}}},
                                                    {.id = 2, .type = "classicShiftGlitch", .params = {}}}});
        std::vector<std::string> warnings;
        azl::dropUnknownEffects(show, warnings);
        CHECK(warnings.empty());
        CHECK(show.layers[0].effects[0].params.at("syncToBeat") == 1.0);
        CHECK(show.layers[0].effects[1].params.at("syncToBeat") == 1.0);
    }

    TempDir presets;
    write(presets.path / "Video Effects/Trails/Smear.xml", R"(<Preset><RenderPass type="DryWetEffect">
  <RenderPass name="Trails" type="Trails"><Params name="Params">
    <ParamRange name="Feedback" default="0.9" value="0.77"/><ParamChoice name="Mode" default="0" value="2"/>
  </Params></RenderPass></RenderPass></Preset>)");
    write(presets.path / "Video Effects/Strobe/Flash.xml", R"(<Preset><RenderPass type="DryWetEffect">
  <RenderPass name="Strobe" type="Strobe"><Params name="Params">
    <ParamRange name="Frequency" default="0.5" value="0.8"/><Param name="Fade" default="0" value="1"/>
    <ParamColor name="Color" default="4294967295" value="4278190335"><Params name="Channels">
      <ParamRange name="Red" default="1" value="1"/><ParamRange name="Green" default="1" value="0"/>
      <ParamRange name="Blue" default="1" value="0"/><ParamRange name="Hue" default="0" value="0"/>
      <ParamRange name="Alpha" default="1" value="0.6"/>
    </Params></ParamColor>
  </Params></RenderPass></RenderPass></Preset>)");
    write(presets.path / "Video Effects/Shift Glitch/Bars.xml", R"(<Preset><RenderPass type="DryWetEffect">
  <RenderPass name="Shift Glitch" type="FFGLEffect"><Params name="Params">
    <ParamRange name="Frequency" default="1" value="0.6"/><ParamRange name="Vertical" default="0" value="0.3"/>
    <ParamRange name="Horizontal" default="0.5" value="0.1"/><ParamRange name="Size" default="0.5" value="0.2"/>
    <Param name="Black BG" default="0" value="1"/>
  </Params></RenderPass>
  <ChoosableMixer name="Mixer"><RenderPass name="Add" type="Add" baseType="Mixer"/></ChoosableMixer>
</RenderPass></Preset>)");
    std::map<std::string, azl::ClassicPreset> byName;
    for (const azl::ClassicPreset& p : azl::readClassicPresets({presets.path}))
        byName[p.name] = p;
    CHECK(byName["Smear"].effectType == "classicTrails");
    CHECK(byName["Smear"].params == std::map<std::string, double>{{"feedback", 0.77}, {"mode", 2.0}});
    CHECK(byName["Flash"].effectType == "classicStrobe");
    CHECK(byName["Flash"].params == std::map<std::string, double>{{"frequency", 0.8},
                                                                  {"fade", 1.0},
                                                                  {"colorRed", 1.0},
                                                                  {"colorGreen", 0.0},
                                                                  {"colorBlue", 0.0},
                                                                  {"colorAlpha", 0.6}});
    CHECK(byName["Bars"].effectType == "classicShiftGlitch");
    CHECK(byName["Bars"].params == std::map<std::string, double>{{"frequency", 0.6},
                                                                 {"vertical", 0.3},
                                                                 {"horizontal", 0.1},
                                                                 {"size", 0.2},
                                                                 {"blackBG", 1.0},
                                                                 {"blendMode", 1.0}});
    for (const char* name : {"Smear", "Flash", "Bars"})
        CHECK(byName[name].skipped.empty());

    azl::EngineHarness harness({.width = 960, .height = 540});
    Client client(harness.document());
    for (const auto& [type, param, value, before] : {std::tuple{"classicTrails", "feedback", 0.4, 0.9},
                                                     {"classicTrails", "mode", 1.0, 0.0},
                                                     {"classicStrobe", "colorGreen", 0.3, 1.0},
                                                     {"classicShiftGlitch", "size", 0.8, 0.5}}) {
        CAPTURE(param);
        const uint64_t effect = addEffect(client, type)["effect"]["id"];
        client.send(msg::command(azl::SetEffectParam{effect, param, value}));
        CHECK(client.next()["value"] == doctest::Approx(value));
        client.send(msg::undo());
        client.next();
        CHECK(harness.document().document().layers[0].effects.back().params.at(param) == before);
    }

    // A Lighten trail of the card moving down and right, and the strobe ramp and the glitch blocks at fixed times.
    {
        azl::EngineHarness golden({.width = 960, .height = 540});
        Client c(golden.document());
        const uint64_t transform = addEffect(c, "transform")["effect"]["id"];
        addEffect(c, "classicTrails", {{"feedback", 0.8}, {"mode", 2.0}});
        for (int i = 0; i < 12; ++i) {
            c.send(msg::command(azl::SetEffectParams{transform, {{"positionX", 0.02 * i}, {"positionY", -0.02 * i}}}));
            c.next();
            golden.step();
        }
        checkGolden(golden.readScreen(), "classic_trails");
    }
}

TEST_CASE("Trails and Classic Trails fade by elapsed time: at 30 frames per second as at 60 at the same moments")
{
    // The history of the test card's white left edge at x = 0 after the card moves away, every 30th of a second.
    auto fade = [](double fps, const std::string& type, std::map<std::string, double> params) {
        azl::EngineHarness harness({.width = 960, .height = 540}, fps);
        Client client(harness.document());
        const uint64_t transform = addEffect(client, "transform")["effect"]["id"];
        addEffect(client, type, std::move(params));
        harness.step();
        client.send(msg::command(azl::SetEffectParam{transform, "positionX", 0.5}));
        client.next();
        const int perSample = int(fps / 30.0);
        std::vector<int> left;
        for (int i = 0; i < 15; ++i) {
            harness.step(perSample);
            left.push_back(pixel(harness.readScreen(), 0, 270)[0]);
        }
        return left;
    };
    for (const auto& [type, params] :
         {std::pair<std::string, std::map<std::string, double>>{"trails", {{"feedback", 0.8}}},
          {"classicTrails", {{"feedback", 0.8}}},
          {"classicTrails", {{"feedback", 0.95}}},
          {"classicTrails", {{"feedback", 0.8}, {"mode", 2.0}}}}) {
        CAPTURE(type);
        CAPTURE(params.at("feedback"));
        const std::vector<int> steady = fade(60.0, type, params), skipped = fade(30.0, type, params);
        CAPTURE(steady);
        CAPTURE(skipped);
        CHECK(steady.front() < 250);
        CHECK(steady.back() < steady.front());
        for (size_t i = 0; i < steady.size(); ++i)
            CHECK(std::abs(steady[i] - skipped[i]) <= 2);
    }
}

TEST_CASE("golden frames of Classic effects")
{
    struct Case {
        const char* golden;
        const char* type;
        std::map<std::string, double> params;
    };
    const Case cases[] = {
        {"classic_stripper", "classicStripper",
         {{"number", 0.6}, {"direction", 0.1}, {"speed", 0.02}, {"feather", 0.3}, {"blue", 0.4}}},
        {"classic_stripper_add", "classicStripper",
         {{"number", 0.5}, {"globalWidth", 0.3}, {"blendMode", 1.0}, {"mode", 0.34}, {"mix", 0.5}}},
        {"classic_stripper_crossing", "classicStripper",
         {{"number", 1.0}, {"minWidth", 0.02}, {"maxWidth", 0.05}, {"directionSpread", 0.5}, {"speed", 0.01},
          {"speedSpread", 0.5}}},
        {"classic_stripper_invert", "classicStripper", {{"number", 0.7}, {"mode", 0.84}, {"globalWidth", 0.2}}},
        {"classic_stripper_zoom", "classicStripper",
         {{"number", 0.4}, {"mode", 0.5}, {"direction", 0.1}, {"globalWidth", 0.5}}},
        {"classic_stripper_zoom_random", "classicStripper", {{"number", 0.4}, {"mode", 0.67}, {"globalWidth", 0.5}}},
        {"classic_stripper_channels", "classicStripper",
         {{"number", 0.4}, {"mode", 1.0}, {"direction", 0.1}, {"globalWidth", 0.5}}},
        {"classic_colorize", "classicColorize",
         {{"colorRed", 0.2}, {"colorGreen", 0.6}, {"colorBlue", 1.0}, {"weighted", 0.0}, {"invert", 1.0},
          {"contrast", 0.5}, {"greysKeep", 1.0}, {"greysTolerance", 0.1}, {"colorAlpha", 0.8}}},
        {"classic_colorize_add", "classicColorize",
         {{"colorRed", 0.0}, {"colorGreen", 0.1}, {"colorBlue", 1.0}, {"blendMode", 1.0}, {"mix", 0.6}}},
        {"classic_colorize_palette", "classicColorize", {{"mode", 1.0}, {"contrast", 0.25}}},
        {"classic_shift_rgb", "classicShiftRgb", {{"mode", 2.0}, {"r", 0.2}, {"g", -0.1}, {"b", 0.05}}},
        {"classic_shift_rgb_zoom", "classicShiftRgb", {{"mode", 5.0}, {"r", -0.05}, {"b", 0.3}, {"blackBG", 1.0}}},
        {"classic_mirror", "classicMirror", {{"x", 0.3}, {"y", 0.6}, {"inOut", 0.6}, {"flipY", 1.0}}},
        {"classic_kaleidoscope", "classicKaleidoscope", {{"angles", 0.45}, {"rotation", 0.1}, {"inputRotation", 0.3}}},
        {"classic_twitch", "classicTwitch",
         {{"maxPosX", 0.3}, {"posFrequency", 1.0}, {"maxLight", 0.5}, {"lightFrequency", 1.0}, {"maxBlur", 0.4},
          {"blurFrequency", 1.0}}},
        {"classic_auto_mask", "classicAutoMask", {{"contrast", 0.8}}},
        {"classic_brightness_contrast", "classicBrightnessContrast", {{"brightness", 0.1}, {"contrast", -0.3}}},
        {"classic_levels", "classicLevels", {{"base", 0.45}, {"midpoint", 0.7}, {"ratio", 0.6}}},
        {"classic_threshold", "classicThreshold",
         {{"threshold", 0.4}, {"smoothness", 0.15}, {"color1Red", 1.0}, {"color1Green", 0.8}, {"color1Blue", 0.2},
          {"color2Blue", 0.5}, {"color2Alpha", 0.6}, {"alpha", 2.0}}},
        {"classic_hue_rotate", "classicHueRotate", {{"hueScale", 0.7}, {"hueRotate", 0.3}, {"satScale", 0.8}}},
        {"classic_hue_rotate_add", "classicHueRotate", {{"hueRotate", 0.5}, {"blendMode", 1.0}, {"mix", 0.7}}},
        {"classic_mirror_quad", "classicMirrorQuad", {{"flipY", 1.0}}},
        {"classic_polar_kaleido", "classicPolarKaleido", {{"rings", 3.0}, {"parts", 5.0}, {"angle", 0.1}}},
        {"classic_strobe_fade", "classicStrobe",
         {{"fade", 1.0}, {"colorRed", 0.2}, {"colorGreen", 0.6}, {"colorAlpha", 0.7}}},
        {"classic_shift_glitch", "classicShiftGlitch", {{"frequency", 0.5}, {"vertical", 0.6}, {"size", 0.7}}},
        {"classic_edge_detection", "classicEdgeDetection", {{"multiplier", 0.3}}},
        {"classic_edge_detection_color", "classicEdgeDetection",
         {{"mode", 0.0}, {"useColor", 1.0}, {"colorRed", 1.0}, {"colorGreen", 0.6}, {"colorBlue", 0.1}}},
    };
    for (const Case& c : cases) {
        CAPTURE(c.golden);
        azl::EngineHarness harness({.width = 960, .height = 540});
        Client client(harness.document());
        addEffect(client, c.type, c.params);
        harness.step(30);
        checkGolden(harness.readScreen(), c.golden);
    }
}

namespace {

struct Score {
    double mae = 0.0, p99 = 0.0, over8 = 0.0, ssim = 0.0;
};

// Both pictures composited over black; a pixel's error is its largest channel difference, SSIM is on Rec. 709 luma
// in 8x8 blocks.
Score score(const azl::Image& a, const azl::Image& b)
{
    const size_t pixels = size_t(a.width) * a.height;
    std::vector<float> la(pixels), lb(pixels);
    std::vector<size_t> histogram(256);
    double sum = 0.0;
    size_t over8 = 0;
    for (size_t i = 0; i < pixels; ++i) {
        float ca[3], cb[3];
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            ca[c] = a.rgba[i * 4 + c] * (a.rgba[i * 4 + 3] / 255.0f);
            cb[c] = b.rgba[i * 4 + c] * (b.rgba[i * 4 + 3] / 255.0f);
            const int d = int(std::lround(std::abs(ca[c] - cb[c])));
            sum += d;
            worst = std::max(worst, d);
        }
        ++histogram[worst];
        over8 += worst > 8;
        la[i] = 0.2126f * ca[0] + 0.7152f * ca[1] + 0.0722f * ca[2];
        lb[i] = 0.2126f * cb[0] + 0.7152f * cb[1] + 0.0722f * cb[2];
    }
    Score s{sum / (pixels * 3.0), 0.0, double(over8) / pixels, 0.0};
    for (size_t seen = 0, level = 0; level < 256; ++level)
        if ((seen += histogram[level]) >= pixels * 0.99) {
            s.p99 = double(level);
            break;
        }
    const double c1 = 6.5025, c2 = 58.5225;
    double total = 0.0;
    int blocks = 0;
    for (uint32_t y = 0; y + 8 <= a.height; y += 8)
        for (uint32_t x = 0; x + 8 <= a.width; x += 8, ++blocks) {
            double ma = 0, mb = 0, va = 0, vb = 0, cov = 0;
            for (uint32_t j = 0; j < 8; ++j)
                for (uint32_t i = 0; i < 8; ++i) {
                    const size_t k = size_t(y + j) * a.width + x + i;
                    ma += la[k];
                    mb += lb[k];
                }
            ma /= 64;
            mb /= 64;
            for (uint32_t j = 0; j < 8; ++j)
                for (uint32_t i = 0; i < 8; ++i) {
                    const size_t k = size_t(y + j) * a.width + x + i;
                    va += (la[k] - ma) * (la[k] - ma);
                    vb += (lb[k] - mb) * (lb[k] - mb);
                    cov += (la[k] - ma) * (lb[k] - mb);
                }
            total += ((2 * ma * mb + c1) * (2 * cov / 63 + c2)) / ((ma * ma + mb * mb + c1) * ((va + vb) / 63 + c2));
        }
    s.ssim = total / blocks;
    return s;
}

} // namespace

// Renders parity cases at 1920x1080 and scores them against reference captures, for the parity scripts:
// NOCTURNE_PARITY_DIR=DIR holding cases.json. Writes DIR/input.png (the bare test card), DIR/ours/NAME.png when
// NOCTURNE_PARITY_SAVE is set, and DIR/scores.json for every case with a DIR/ref/NAME.png. `shown` is the share of the
// picture left as it was, for effects whose layout is random in the reference.
TEST_CASE("parity frames of Classic effects")
{
    const char* dir = std::getenv("NOCTURNE_PARITY_DIR");
    if (!dir) {
        MESSAGE("skipped: set NOCTURNE_PARITY_DIR");
        return;
    }
    const fs::path root(dir);
    std::ifstream in(root / "cases.json");
    const nlohmann::json work = nlohmann::json::parse(in);
    const nlohmann::json& cases = work["cases"];
    const std::string inputName = work.value("input", "input.png");
    const bool probe = kProbes.contains(inputName);
    azl::EngineHarness harness({.width = 1920, .height = 1080});
    Client client(harness.document());
    if (probe)
        addEffect(client, probeEffect(inputName));
    harness.step();
    const azl::Image input = harness.readScreen();
    savePng(input, (root / inputName).string());
    fs::create_directories(root / "ours");
    nlohmann::json scores = nlohmann::json::object();
    auto record = [&](const std::string& name, const azl::Image& ours) {
        std::optional<azl::Image> ref = loadPng((root / "ref" / (name + ".png")).string());
        if (!ref || ref->width != ours.width || ref->height != ours.height)
            return;
        // loadPng premultiplies, which is the picture over black already.
        for (size_t i = 3; i < ref->rgba.size(); i += 4)
            ref->rgba[i] = 255;
        const Score s = score(ours, *ref);
        scores[name] = {{"mae", s.mae},   {"p99", s.p99}, {"over8", s.over8}, {"ssim", s.ssim},
                        {"shown", 1.0 - score(ours, input).over8}, {"shownRef", 1.0 - score(*ref, input).over8}};
    };
    record("input", input);
    // The test card is drawn, not read from a texture; an effect that reads its input in several places (a Shift RGB
    // at its defaults) in front makes a remap read texels the way it does on a clip.
    if (!probe)
        addEffect(client, "classicShiftRgb");
    std::string type;
    uint64_t effect = 0;
    for (const auto& c : cases) {
        const std::string name = c["name"];
        if (!azl::findEffect(c["type"]) || c.value("sequence", false))
            continue;
        if (c["type"] != type) {
            if (effect) {
                client.send(msg::command(azl::RemoveEffect{effect}));
                client.next();
            }
            type = c["type"];
            effect = addEffect(client, type)["effect"]["id"];
        }
        std::map<std::string, double> params;
        for (const azl::ParamDef& p : azl::findEffect(type)->params)
            params[p.name] = p.defaultValue;
        for (const auto& [key, value] : c["params"].items())
            params[key] = value.get<double>();
        client.send(msg::command(azl::SetEffectParams{effect, params}));
        client.next();
        // The test card's marker returns to where the input still has it every 2 s.
        harness.step(120);
        const azl::Image ours = harness.readScreen();
        if (std::getenv("NOCTURNE_PARITY_SAVE"))
            savePng(ours, (root / "ours" / (name + ".png")).string());
        record(name, ours);
    }
    std::ofstream(root / "scores.json") << scores.dump(1);
}

namespace {

double meanLuma(const azl::Image& image)
{
    double sum = 0.0;
    for (size_t i = 0; i < image.rgba.size(); i += 4)
        sum += (0.2126 * image.rgba[i] + 0.7152 * image.rgba[i + 1] + 0.0722 * image.rgba[i + 2]) * image.rgba[i + 3];
    return sum / 255.0 / double(image.rgba.size() / 4);
}

// The marker clip's frame number: 8 blocks along the top, low bit first, white = 1; -1 when a block is not clean.
int markerFrame(const azl::Image& image)
{
    int frame = 0;
    for (int bit = 0; bit < 8; ++bit) {
        const int level = pixel(image, 80 + bit * 100, 50)[0];
        if (level > 60 && level < 195)
            return -1;
        frame |= int(level >= 128) << bit;
    }
    return frame;
}

nlohmann::json scoreJson(const Score& s)
{
    return {{"mae", s.mae}, {"p99", s.p99}, {"over8", s.over8}, {"ssim", s.ssim}};
}

// A reference snapshot over black, as `record` reads it.
azl::Image refFrame(const fs::path& file)
{
    std::optional<azl::Image> ref = loadPng(file.string());
    REQUIRE(ref);
    for (size_t i = 3; i < ref->rgba.size(); i += 4)
        ref->rgba[i] = 255;
    return *ref;
}

std::pair<std::vector<int>, std::vector<int>> medianLines(const azl::Image& image)
{
    std::vector<int> rowG(image.height), colR(image.width), line;
    for (uint32_t y = 0; y < image.height; ++y) {
        line.clear();
        for (uint32_t x = 0; x < image.width; ++x)
            line.push_back(pixel(image, x, y)[1]);
        std::nth_element(line.begin(), line.begin() + line.size() / 2, line.end());
        rowG[y] = line[line.size() / 2];
    }
    for (uint32_t x = 0; x < image.width; ++x) {
        line.clear();
        for (uint32_t y = 0; y < image.height; ++y)
            line.push_back(pixel(image, x, y)[0]);
        std::nth_element(line.begin(), line.begin() + line.size() / 2, line.end());
        colR[x] = line[line.size() / 2];
    }
    return {rowG, colR};
}

} // namespace

// Renders the timed parity cases (`sequence` in DIR/cases.json) for the parity scripts: each reference sequence
// DIR/seq/NAME.I is matched by one of ours with a fresh effect switched on at its start and sampled at the reference's
// snapshot times (Strobe every frame for 8 s). Writes DIR/ours/seq/NAME.I.json: per sample its time, mean colour over
// black, alpha, the share changed from the bare input by more than 8 levels and whether it equals the last sample, and
// on the UV probe the median G of each row and R of each column; our brightest and darkest samples against the
// reference's; and, on the marker clip with the clip phase of the sequence in DIR/phase.json, each reference snapshot
// after 1.5 s against our frame of the same clip time (the best within 70 ms). NOCTURNE_PARITY_ONLY limits it to case
// names containing that text; NOCTURNE_PARITY_INSTANCES=N renders at least N instances of ours, and
// NOCTURNE_PARITY_SAVE keeps our first samples and matched frames as PNGs.
TEST_CASE("parity sequences of Classic effects")
{
    const char* dir = std::getenv("NOCTURNE_PARITY_DIR");
    if (!dir) {
        MESSAGE("skipped: set NOCTURNE_PARITY_DIR");
        return;
    }
    const fs::path root(dir);
    std::ifstream in(root / "cases.json");
    const nlohmann::json work = nlohmann::json::parse(in);
    const std::string inputName = work.value("input", "input.png");
    const bool probe = kProbes.contains(inputName);
    const bool clip = inputName.ends_with(".mp4");
    nlohmann::json phases = nlohmann::json::object();
    if (std::ifstream file(root / "phase.json"); file)
        phases = nlohmann::json::parse(file);
    const char* only = std::getenv("NOCTURNE_PARITY_ONLY");
    const char* instances = std::getenv("NOCTURNE_PARITY_INSTANCES");
    const int extraInstances = instances ? std::atoi(instances) : 0;
    fs::create_directories(root / "ours/seq");
    for (const auto& c : work["cases"]) {
        const std::string name = c["name"], type = c["type"];
        if (!c.value("sequence", false) || !azl::findEffect(type) || (only && name.find(only) == std::string::npos))
            continue;
        std::map<std::string, double> params;
        for (const azl::ParamDef& p : azl::findEffect(type)->params)
            params[p.name] = p.defaultValue;
        for (const auto& [key, value] : c["params"].items())
            params[key] = value.get<double>();
        // The reference times in seconds.
        if (params.contains("syncToBeat"))
            params["syncToBeat"] = 0.0;
        for (int instance = 0;; ++instance) {
            const std::string seqName = name + "." + std::to_string(instance);
            // Instances beyond the reference's take the timing of its first.
            const bool extra = !fs::exists(root / "seq" / seqName / "timeline.json");
            if (extra && (instance == 0 || instance >= extraInstances))
                break;
            const fs::path seq = root / "seq" / (extra ? name + ".0" : seqName);
            CAPTURE(seqName);
            const nlohmann::json meta = nlohmann::json::parse(std::ifstream(seq / "frames.json"));
            const nlohmann::json& frames = meta["frames"];
            const nlohmann::json timeline = nlohmann::json::parse(std::ifstream(seq / "timeline.json"));
            azl::EngineHarness harness({.width = 1920, .height = 1080});
            Client client(harness.document());
            // The reference's tempo while it was captured.
            if (meta.contains("tempo") && meta["tempo"].is_number())
                apply(client, azl::SetTempo{meta["tempo"].get<double>()});
            if (probe)
                addEffect(client, probeEffect(inputName));
            else if (clip)
                playMedia(client, root / inputName);
            harness.step();
            const azl::Image input = harness.readScreen();
            // Where the clip's frame changes, to give each of our frames its clip time.
            double clipStart = 0.0;
            if (clip) {
                const int first = markerFrame(input);
                int shown = first;
                while (shown == first) {
                    harness.step();
                    shown = markerFrame(harness.readScreen());
                }
                clipStart = harness.time() - 1.0 / 60.0 - shown / 30.0;
            } else if (!probe) {
                addEffect(client, "classicShiftRgb");
            }
            // Each instance starts 5 s further into the beat clock, so no two see the same beat-timed pattern.
            harness.step(instance * 300);
            std::optional<double> phase;
            if (clip && !extra && phases.contains(seqName))
                phase = phases[seqName].get<double>();
            // Switched on at the clip time the reference's was, which a history that never forgets depends on.
            while (phase && std::abs(std::remainder(harness.time() - clipStart - *phase, 4.0)) > 0.5 / 60.0)
                harness.step();
            const uint64_t effect = addEffect(client, type, params)["effect"]["id"];
            const double start = harness.time();

            std::vector<std::pair<double, size_t>> targets;
            for (size_t i = 0; i < timeline.size(); ++i)
                if (phase && !timeline[i]["overlaid"].get<bool>() && timeline[i]["t"].get<double>() >= 1.5)
                    targets.push_back({std::fmod(timeline[i]["t"].get<double>() + *phase, 4.0), i});
            std::vector<nlohmann::json> best(timeline.size());

            nlohmann::json samples = nlohmann::json::array();
            std::optional<azl::Image> last, bright, dark;
            double brightLuma = -1.0, darkLuma = 1e9;
            // A periodic effect is sampled every frame for twice as long, so its phase can be fitted.
            const bool periodic = type == "classicStrobe";
            const double end = periodic ? 8.1 : targets.empty() ? timeline.back()["t"].get<double>() + 0.05 : 9.6;
            size_t next = 0;
            for (double t = 0.0; t <= end; t = harness.time() - start) {
                harness.step();
                const bool sampled =
                    periodic || (next < timeline.size() && t + 0.5 / 60.0 >= timeline[next]["t"].get<double>());
                std::vector<size_t> matched;
                const double clipTime = std::fmod(t + start - clipStart + 400.0, 4.0);
                for (const auto& [target, i] : targets)
                    if (t >= 1.5 && std::abs(std::remainder(clipTime - target, 4.0)) <= 0.07)
                        matched.push_back(i);
                if (!sampled && matched.empty())
                    continue;
                const azl::Image ours = harness.readScreen();
                for (const size_t i : matched) {
                    const Score s = score(ours, refFrame(seq / frames[i]["file"].get<std::string>()));
                    if (!best[i].is_null() && s.mae >= best[i]["mae"].get<double>())
                        continue;
                    best[i] = {{"file", frames[i]["file"]}, {"t", timeline[i]["t"]}, {"at", t}, {"mae", s.mae},
                               {"ssim", s.ssim}, {"p99", s.p99}};
                    if (std::getenv("NOCTURNE_PARITY_SAVE"))
                        savePng(ours, (root / "ours/seq" / (seqName + ".match." + frames[i]["file"].get<std::string>()))
                                          .string());
                }
                if (!sampled)
                    continue;
                ++next;
                double mean[3] = {0.0, 0.0, 0.0}, alpha = 0.0;
                for (size_t k = 0; k < ours.rgba.size(); k += 4) {
                    for (int ch = 0; ch < 3; ++ch)
                        mean[ch] += ours.rgba[k + ch] * (ours.rgba[k + 3] / 255.0);
                    alpha += ours.rgba[k + 3] / 255.0;
                }
                const double pixels = double(ours.rgba.size() / 4);
                nlohmann::json sample = {{"t", t},
                                         {"mean", {mean[0] / pixels, mean[1] / pixels, mean[2] / pixels}},
                                         {"alpha", alpha / pixels},
                                         {"changed", score(ours, input).over8},
                                         {"same", last && last->rgba == ours.rgba}};
                if (probe) {
                    const auto [rowG, colR] = medianLines(ours);
                    sample["rowG"] = rowG;
                    sample["colR"] = colR;
                }
                samples.push_back(sample);
                const double luma = meanLuma(ours);
                if (luma > brightLuma) {
                    brightLuma = luma;
                    bright = ours;
                }
                if (luma < darkLuma) {
                    darkLuma = luma;
                    dark = ours;
                }
                if (std::getenv("NOCTURNE_PARITY_SAVE") && samples.size() <= 3)
                    savePng(ours, (root / "ours/seq" / (seqName + "." + std::to_string(samples.size() - 1) + ".png"))
                                      .string());
                last = ours;
            }
            client.send(msg::command(azl::RemoveEffect{effect}));
            client.next();

            // The reference's brightest and darkest snapshots, overlaid ones skipped.
            size_t refBright = 0, refDark = 0;
            double hi = -1.0, lo = 1e9;
            for (size_t i = 0; i < timeline.size(); ++i) {
                if (timeline[i]["overlaid"].get<bool>())
                    continue;
                const auto& m = timeline[i]["mean"];
                const double luma =
                    0.2126 * m[0].get<double>() + 0.7152 * m[1].get<double>() + 0.0722 * m[2].get<double>();
                if (luma > hi) {
                    hi = luma;
                    refBright = i;
                }
                // A snapshot the overlay left translucent without being flagged is not the darkest picture.
                if (luma < lo && timeline[i]["alpha"].get<double>() >= 0.99) {
                    lo = luma;
                    refDark = i;
                }
            }
            nlohmann::json out = {{"case", name}, {"instance", instance}, {"samples", samples}};
            if (bright && dark && !extra) {
                out["bright"] = scoreJson(score(*bright, refFrame(seq / frames[refBright]["file"].get<std::string>())));
                out["dark"] = scoreJson(score(*dark, refFrame(seq / frames[refDark]["file"].get<std::string>())));
            }
            if (phase) {
                out["matches"] = nlohmann::json::array();
                for (const nlohmann::json& match : best)
                    if (!match.is_null())
                        out["matches"].push_back(match);
            }
            std::ofstream(root / "ours/seq" / (seqName + ".json")) << out.dump();
            MESSAGE(seqName << ": " << samples.size() << " samples");
        }
    }
}
