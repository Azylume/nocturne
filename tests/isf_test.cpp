#include "document/composition.hpp"
#include "effects/colour.hpp"
#include "effects/isf.hpp"
#include "engine/isf_compiler.hpp"
#include "engine/shader_cross.hpp"

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

const azl::IsfSource kTint{"Test Tint", R"(/*{
    "ISFVSN": "2",
    "INPUTS": [
        {"NAME": "inputImage", "TYPE": "image"},
        {"NAME": "amount", "TYPE": "float", "MIN": 0.0, "MAX": 2.0, "DEFAULT": 0.5, "LABEL": "Amount"},
        {"NAME": "invert", "TYPE": "bool", "DEFAULT": true},
        {"NAME": "mode", "TYPE": "long", "VALUES": [2, 5, 9], "LABELS": ["Two", "Five", "Nine"], "DEFAULT": 5},
        {"NAME": "tint", "TYPE": "color", "DEFAULT": [1.0, 0.5, 0.25, 1.0]},
        {"NAME": "centre", "TYPE": "point2D", "MIN": [0, 0], "MAX": [1, 1], "DEFAULT": [0.25, 0.75]}
    ]
}*/
void main() {
    vec4 c = IMG_THIS_PIXEL(inputImage);
    float d = distance(isf_FragNormCoord, centre) * float(mode);
    c.rgb = mix(c.rgb, tint.rgb, amount * d);
    gl_FragColor = invert ? vec4(1.0 - c.rgb, c.a) : c;
}
)"};

const azl::IsfSource kShift{"Test Shift", R"(/*{
    "INPUTS": [{"NAME": "inputImage", "TYPE": "image"}, {"NAME": "shift", "TYPE": "float"}]
}*/
void main() { gl_FragColor = IMG_NORM_PIXEL(inputImage, isf_FragNormCoord + vec2(shift, 0.0)); }
)"};

azl::IsfShader parsed(const azl::IsfSource& source)
{
    auto result = azl::parseIsf(source);
    if (auto* error = std::get_if<std::string>(&result))
        FAIL(*error);
    return std::get<azl::IsfShader>(result);
}

std::string parseError(const std::string& text)
{
    auto result = azl::parseIsf({"Broken", text});
    const auto* error = std::get_if<std::string>(&result);
    return error ? *error : "";
}

std::filesystem::path freshDir(const char* name)
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / (std::string(name) + "-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    return dir;
}

} // namespace

TEST_CASE("a GLSL fragment shader crosses to MSL; a broken one reports the compiler's error")
{
    const azl::CrossResult ok = azl::fragmentGlslToMsl(R"(#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;
void main() { colour = vec4(uv, 0.0, 1.0); }
)");
    CHECK(ok.error.empty());
    CHECK(ok.msl.find("fragment") != std::string::npos);

    const azl::CrossResult broken = azl::fragmentGlslToMsl("#version 450\nvoid main() { undefinedThing = 1.0; }\n");
    CHECK(broken.msl.empty());
    CHECK(broken.error.find("undefinedThing") != std::string::npos);
}

TEST_CASE("an ISF header gives the shader's kind and inputs; what it cannot run is an error")
{
    const azl::IsfShader tint = parsed(kTint);
    CHECK(tint.kind == azl::IsfKind::Effect);
    CHECK(tint.pointwise);
    REQUIRE(tint.inputs.size() == 5);
    CHECK(tint.inputs[2].values == std::vector<int>{2, 5, 9});
    CHECK(tint.params.size() == 1 + 1 + 1 + 4 + 2);
    CHECK_FALSE(parsed(kShift).pointwise);
    CHECK(azl::isfType("RGB EQ") == "isfRgbEq");
    CHECK(azl::isfType("Chromatic Aberration") == "isfChromaticAberration");

    CHECK(parsed({"Gen", "/*{\"INPUTS\": [{\"NAME\": \"a\", \"TYPE\": \"float\"}]}*/ void main() {}"}).kind ==
          azl::IsfKind::Generator);
    CHECK(parseError("void main() {}") == "no JSON header comment");
    CHECK(parseError("/* not json */ void main() {}") == "the header is not a JSON object");
    CHECK(parseError(R"(/*{"INPUTS": [{"NAME": "s", "TYPE": "audio"}]}*/)") ==
          "input s of type audio is not supported");
    CHECK(parseError(R"(/*{"PASSES": [{"TARGET": "a"}, {}]}*/)") == "multi-pass shaders are not supported");
    CHECK(parseError(R"(/*{"INPUTS": [{"NAME": "p", "TYPE": "point2D"}]}*/)") == "point input p needs MIN and MAX");
    CHECK(parseError(R"(/*{"INPUTS": [{"NAME": "mix", "TYPE": "float"}]}*/)") == "parameter name mix is taken");
    CHECK(parseError(R"(/*{"INPUTS": [{"NAME": "startImage", "TYPE": "image"}, {"NAME": "endImage", "TYPE": "image"},
        {"NAME": "progress", "TYPE": "float"}]}*/
vec4 transition(vec2 uv) { return mix(IMG_NORM_PIXEL(startImage, uv * 0.5), IMG_NORM_PIXEL(endImage, uv), progress); }
void main() { gl_FragColor = transition(isf_FragNormCoord.xy); })") ==
          "a transition must read both pictures at its own pixel only");
}

TEST_CASE("an ISF shader translates to namespaced Metal and caches it on disk; a broken one reports its error")
{
    const std::filesystem::path dir = freshDir("isf-cache-test");
    const azl::IsfMetal tint = azl::translateIsf(parsed(kTint), dir);
    REQUIRE_MESSAGE(tint.error.empty(), tint.error);
    CHECK(tint.globals.find("namespace isf_isfTestTint {") != std::string::npos);
    CHECK(tint.globals.find("[[") == std::string::npos);
    CHECK(tint.body.find("isf_isfTestTint::main0(i)") != std::string::npos);
    CHECK(azl::translateIsf(parsed(kShift), {}).body.find("main0(i, t, lin)") != std::string::npos);

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        files.push_back(entry.path());
    REQUIRE(files.size() == 1);
    std::ofstream(files[0]) << "struct main0_in {}; // from the cache\n";
    CHECK(azl::translateIsf(parsed(kTint), dir).globals.find("from the cache") != std::string::npos);
    std::filesystem::remove_all(dir);

    azl::IsfShader broken = parsed(kShift);
    broken.glsl = "void main() { gl_FragColor = vec4(missingValue); }";
    const azl::IsfMetal failed = azl::translateIsf(broken, {});
    CHECK(failed.globals.empty());
    CHECK(failed.error.find("missingValue") != std::string::npos);
}

TEST_CASE("an ISF shader read from the cache is the same Metal as one translated, for each type it registers as")
{
    const std::filesystem::path dir = freshDir("isf-cache-same-test");
    const azl::IsfShader tint = parsed(kTint);
    azl::IsfShader renamed = tint;
    renamed.type = "isfTestTintToo";
    const azl::IsfMetal fresh = azl::translateIsf(tint, {});
    REQUIRE_MESSAGE(fresh.error.empty(), fresh.error);
    CHECK(azl::translateIsf(tint, dir).globals == fresh.globals);
    CHECK(azl::translateIsf(tint, dir).globals == fresh.globals);
    CHECK(azl::translateIsf(renamed, dir).globals == azl::translateIsf(renamed, {}).globals);
    std::filesystem::remove_all(dir);
}

TEST_CASE("ISF inputs become parameters that colour pickers, macros and modulators work on")
{
    azl::EffectDef def = azl::isfEffect(parsed(kTint));
    CHECK(def.type == "isfTestTint");
    CHECK(def.category == "ISF");
    CHECK(def.kind == azl::StageKind::Pointwise);
    std::vector<std::string> names;
    for (const azl::ParamDef& p : def.params)
        names.push_back(p.name);
    CHECK(names == std::vector<std::string>{"amount", "invert", "mode", "tintRed", "tintGreen", "tintBlue", "tintAlpha",
                                            "centreX", "centreY"});
    CHECK(def.params[0].max == 2.0);
    CHECK(def.params[0].defaultValue == 0.5);
    CHECK(def.params[0].label == "Amount");
    CHECK(def.params[1].choice);
    CHECK(def.params[1].defaultValue == 1.0);
    CHECK(def.params[2].options == std::vector<std::string>{"Two", "Five", "Nine"});
    CHECK(def.params[2].defaultValue == 1.0);
    CHECK(def.params[4].defaultValue == 0.5);
    CHECK(def.params[8].defaultValue == 0.75);
    REQUIRE(azl::colourGroups(def).size() == 1);
    CHECK(azl::colourGroups(def)[0].alpha == "tintAlpha");
    CHECK(azl::isfEffect(parsed(kShift)).kind == azl::StageKind::Neighbourhood);

    // Registered without its colour, so the tests that check every effect's colour groups do not see it.
    def.type = "testIsfTint";
    def.params.erase(def.params.begin() + 3, def.params.begin() + 7);
    if (!azl::findEffect(def.type))
        azl::registerEffect(def);
    CHECK(azl::effectLabel("testIsfTint") == "Test Tint");
    azl::Composition composition = azl::defaultComposition();
    azl::Edit add = azl::AddEffect{1, std::nullopt, {0, "testIsfTint", {{"mode", 2.0}}}};
    azl::applyEdit(composition, add);
    const uint64_t effect = composition.layers[0].effects.at(0).id;
    CHECK(composition.layers[0].effects[0].params.at("centreY") == 0.75);
    CHECK(composition.layers[0].effects[0].params.at("mode") == 2.0);
    azl::Edit macro = azl::AddMacro{std::nullopt, {.name = "Amount", .value = 0.5}};
    azl::applyEdit(composition, macro);
    azl::Edit target =
        azl::AddMacroTarget{composition.macros.at(0).id, std::nullopt, {{1, effect, "amount"}, {}, 0.0, 2.0}};
    azl::applyEdit(composition, target);
    azl::Modulator lfo;
    lfo.target = {0, {1, effect, "centreX"}};
    azl::Edit modulator = azl::AddModulator{std::nullopt, lfo};
    azl::applyEdit(composition, modulator);
    CHECK(composition.macros.at(0).targets.size() == 1);
    CHECK(composition.modulators.size() == 1);
}
