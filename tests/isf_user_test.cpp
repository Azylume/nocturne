#include "golden.hpp"
#include "test_client.hpp"

#include "effects/isf.hpp"

#include <doctest.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

namespace {

using Params = std::map<std::string, double>;

constexpr azl::EngineConfig kGolden{.width = 480, .height = 270};

constexpr const char* kTint = R"(/*{
    "DESCRIPTION": "Tints the picture",
    "INPUTS": [
        {"NAME": "inputImage", "TYPE": "image"},
        {"NAME": "tint", "TYPE": "color", "DEFAULT": [1.0, 0.5, 0.0, 1.0]},
        {"NAME": "amount", "TYPE": "float", "MIN": 0.0, "MAX": 1.0, "DEFAULT": 0.5}
    ]
}*/
void main() {
    vec4 c = IMG_THIS_PIXEL(inputImage);
    gl_FragColor = vec4(mix(c.rgb, c.rgb * tint.rgb, amount), c.a);
}
)";

constexpr const char* kRings = R"(/*{
    "INPUTS": [{"NAME": "rings", "TYPE": "float", "MIN": 1.0, "MAX": 20.0, "DEFAULT": 6.0}]
}*/
void main() {
    float v = step(0.5, fract(distance(isf_FragNormCoord, vec2(0.5)) * rings));
    gl_FragColor = vec4(v, v * 0.5, 1.0 - v, 1.0);
}
)";

// The incoming picture left of progress * scale of the width.
constexpr const char* kSplit = R"(/*{
    "INPUTS": [
        {"NAME": "startImage", "TYPE": "image"},
        {"NAME": "endImage", "TYPE": "image"},
        {"NAME": "progress", "TYPE": "float"},
        {"NAME": "scale", "TYPE": "float", "MIN": 0.0, "MAX": 2.0, "DEFAULT": 1.0}
    ]
}*/
vec4 getFromColor(vec2 uv) { return IMG_NORM_PIXEL(startImage, uv); }
vec4 getToColor(vec2 uv) { return IMG_NORM_PIXEL(endImage, uv); }
vec4 transition(vec2 uv) { return uv.x < progress * scale ? getToColor(uv) : getFromColor(uv); }
void main() { gl_FragColor = transition(isf_FragNormCoord.xy); }
)";

constexpr const char* kBroken = R"(/*{"INPUTS": [{"NAME": "inputImage", "TYPE": "image"}]}*/
void main() { gl_FragColor = IMG_THIS_PIXEL(inputImage) * notDeclared; }
)";

void write(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

Message request(Client& client, const std::string& op)
{
    client.send({{"type", "isf"}, {"op", op}, {"text", true}});
    return client.next();
}

// A rescan that changes something also tells every client, the one asking too.
Message rescan(Client& client)
{
    const Message reply = request(client, "rescan");
    CHECK(client.next()["op"] == "list");
    return reply;
}

const Message* shader(const Message& list, const std::string& file)
{
    for (const Message& s : list["shaders"])
        if (s["file"] == file)
            return &s;
    return nullptr;
}

azl::ClipRef solidClip(double red, double green, double blue)
{
    return {.name = "solid", .effects = {azl::Effect{0, "solid", {{"red", red}, {"green", green}, {"blue", blue}}}}};
}

// Layer 1 plays a red generator from column 1, column 2 holds a blue one; triggers land at once.
void redThenBlue(Client& client)
{
    apply(client, azl::SetQuantise{0});
    apply(client, azl::SetSlot{1, 1, solidClip(1.0, 0.0, 0.0)});
    apply(client, azl::SetSlot{1, 2, solidClip(0.0, 0.0, 1.0)});
    client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", 1}, {"column", 1}});
    REQUIRE(client.next()["op"] == "triggerClip");
}

// Halfway through a transition of two beats into the column, 30 frames at 120 BPM and 60 fps.
azl::Image halfway(azl::EngineHarness& harness, Client& client, uint64_t column = 2)
{
    client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", 1}, {"column", column}});
    REQUIRE(client.next()["op"] == "triggerClip");
    harness.step(31);
    return harness.readScreen();
}

bool blue(const azl::Image& image, uint32_t x)
{
    const auto p = pixel(image, x, image.height / 2);
    return p[2] > 200 && p[0] < 50;
}

bool red(const azl::Image& image, uint32_t x)
{
    const auto p = pixel(image, x, image.height / 2);
    return p[0] > 200 && p[2] < 50;
}

size_t differing(const azl::Image& a, const azl::Image& b)
{
    size_t count = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4)
        count += std::abs(a.rgba[i] - b.rgba[i]) + std::abs(a.rgba[i + 1] - b.rgba[i + 1]) +
                     std::abs(a.rgba[i + 2] - b.rgba[i + 2]) >
                 24;
    return count;
}

} // namespace

TEST_CASE("user ISF shaders in the state folder join as user effects, generators and transitions")
{
    TempDir root;
    write(root.path / "isf-user/Test Tint.fs", kTint);
    write(root.path / "isf-user/Test Rings.fs", kRings);
    write(root.path / "isf-user/Test Split.fs", kSplit);
    write(root.path / "isf-user/notes.txt", "not a shader");
    {
        azl::EngineHarness harness(kGolden, 60.0, root.path);
        Client client(harness.document());
        REQUIRE(azl::findEffect("userTestTint"));
        CHECK(azl::findEffect("userTestTint")->category == "User");
        CHECK(azl::effectLabel("userTestTint") == "Test Tint");
        CHECK(azl::isGenerator("userTestRings"));
        const azl::TransitionMode split = azl::userTransitionMode("userTestSplit");
        const auto transitions = azl::isfTransitions();
        const azl::IsfTransition* transition = azl::findIsfTransition(*transitions, split);
        REQUIRE(transition);
        CHECK(transition->user);
        CHECK(transition->params.size() == 1);
        CHECK(azl::transitionName(split) == "userTestSplit");

        const Message list = request(client, "list");
        CHECK(list["folder"] == (root.path / "isf-user").string());
        CHECK(list["shaders"].size() == 3);
        REQUIRE(shader(list, "Test Split.fs"));
        CHECK((*shader(list, "Test Split.fs"))["kind"] == "transition");
        CHECK((*shader(list, "Test Split.fs"))["error"] == "");
        CHECK((*shader(list, "Test Tint.fs"))["text"] == kTint);

        apply(client, azl::AddEffect{1, std::nullopt, {0, "userTestTint", {{"amount", 0.8}}}});
        harness.step(30);
        checkGolden(harness.readScreen(), "userIsfTint");
    }
    // Gone with the engine that read them.
    CHECK_FALSE(azl::findEffect("userTestTint"));
    CHECK_FALSE(azl::findIsfTransition(*azl::isfTransitions(), azl::userTransitionMode("userTestSplit")));
}

TEST_CASE("a broken user ISF file reports its error and the engine keeps rendering")
{
    TempDir root;
    write(root.path / "isf-user/Broken.fs", kBroken);
    write(root.path / "isf-user/No Header.fs", "void main() { gl_FragColor = vec4(1.0); }");
    write(root.path / "isf-user/Too Big.fs", std::string(azl::UserIsfLibrary::kMaxFileBytes + 1, ' '));
    azl::EngineHarness harness({.width = 160, .height = 90}, 60.0, root.path);
    Client client(harness.document());
    const Message list = request(client, "list");
    REQUIRE(shader(list, "Broken.fs"));
    CHECK((*shader(list, "Broken.fs"))["error"].get<std::string>().find("notDeclared") != std::string::npos);
    CHECK((*shader(list, "No Header.fs"))["error"] == "no JSON header comment");
    CHECK((*shader(list, "Too Big.fs"))["error"].get<std::string>().find("larger than") != std::string::npos);
    CHECK_FALSE(azl::findEffect("userNoHeader"));

    harness.step();
    apply(client, azl::AddEffect{1, std::nullopt, {0, "userBroken", {}}});
    const uint64_t rendered = harness.stats().rendered;
    harness.step(3);
    Message failure = client.next();
    while (failure["type"] != "error")
        failure = client.next();
    CHECK(failure["chain"] == std::vector<std::string>{"userBroken"});
    CHECK(harness.stats().rendered == rendered + 3);
}

TEST_CASE("a rescan takes in new, changed and removed user ISF files without a restart")
{
    TempDir root;
    azl::EngineHarness harness({.width = 160, .height = 90}, 60.0, root.path);
    Client client(harness.document());
    Client watcher(harness.document());
    CHECK(request(client, "list")["shaders"].empty());
    CHECK(fs::is_directory(root.path / "isf-user"));

    write(root.path / "isf-user/Test Tint.fs", kTint);
    const Message added = rescan(client);
    CHECK(added["shaders"].size() == 1);
    REQUIRE(azl::findEffect("userTestTint"));
    // Every client hears of the change, with the sources.
    Message heard = watcher.next();
    while (heard["type"] != "isf")
        heard = watcher.next();
    CHECK(heard["op"] == "list");
    CHECK(heard["shaders"][0]["text"] == kTint);

    apply(client, azl::AddEffect{1, std::nullopt, {0, "userTestTint", {{"amount", 1.0}, {"tintGreen", 0.0}}}});
    harness.step(4);
    harness.engine().waitForCompiles();
    harness.step(2);
    const azl::Image screen = harness.readScreen();
    const auto tinted = pixel(screen, 1, 1);
    CHECK(tinted[1] < 5);

    // The shader changes: the chain compiles again and the picture follows.
    std::string untinted = kTint;
    untinted.replace(untinted.find("c.rgb * tint.rgb"), 16, "c.rgb");
    write(root.path / "isf-user/Test Tint.fs", untinted);
    rescan(client);
    harness.engine().waitForCompiles();
    harness.step(2);
    CHECK(pixel(harness.readScreen(), 1, 1)[1] > 250);

    fs::remove(root.path / "isf-user/Test Tint.fs");
    CHECK(rescan(client)["shaders"].empty());
    CHECK_FALSE(azl::findEffect("userTestTint"));
    Message unchanged = request(client, "rescan");
    while (unchanged.value("type", "") != "isf")
        unchanged = client.next();
    CHECK(unchanged["op"] == "rescan");
}

TEST_CASE("a user ISF transition takes its parameters, and switching to it compiles nothing")
{
    TempDir root;
    azl::EngineHarness harness({.width = 160, .height = 90}, 60.0, root.path);
    Client client(harness.document());
    redThenBlue(client);
    harness.step(10);
    write(root.path / "isf-user/Test Split.fs", kSplit);
    rescan(client);
    harness.engine().waitForCompiles();
    harness.step(2);
    const uint64_t pipelines = harness.compileStats().pipelines;

    const azl::TransitionMode split = azl::userTransitionMode("userTestSplit");
    apply(client, azl::SetLayerTransition{1, split, 2.0});
    azl::Image image = halfway(harness, client);
    CHECK(blue(image, 16));
    CHECK(red(image, 144));

    // Twice the progress: the incoming red covers it all halfway.
    harness.step(60);
    apply(client, azl::SetTransitionParam{1, split, "scale", 2.0});
    image = halfway(harness, client, 1);
    CHECK(red(image, 16));
    CHECK(red(image, 144));
    CHECK(harness.compileStats().pipelines == pipelines);

    client.send({{"type", "command"}, {"op", "setTransitionParam"}, {"layer", 1}, {"mode", "userTestSplit"},
                 {"name", "scale"}, {"value", 3.0}});
    CHECK(client.next()["type"] == "error");
    client.send({{"type", "command"}, {"op", "setTransitionParam"}, {"layer", 1}, {"mode", "alpha"},
                 {"name", "scale"}, {"value", 1.0}});
    CHECK(client.next()["type"] == "error");
}

TEST_CASE("ISF transition parameters change the picture, survive save and load, and are undoable")
{
    azl::Image plain;
    {
        azl::EngineHarness harness({.width = 160, .height = 90});
        Client client(harness.document());
        redThenBlue(client);
        harness.step(10);
        apply(client, azl::SetLayerTransition{1, azl::TransitionMode::WindowSlice, 2.0});
        plain = halfway(harness, client);
    }
    azl::Composition saved;
    azl::Image changed;
    {
        azl::EngineHarness harness({.width = 160, .height = 90});
        Client client(harness.document());
        redThenBlue(client);
        harness.step(10);
        apply(client, azl::SetLayerTransition{1, azl::TransitionMode::WindowSlice, 2.0});
        const uint64_t pipelines = harness.compileStats().pipelines;
        apply(client, azl::SetTransitionParam{1, azl::TransitionMode::WindowSlice, "count", 2.0});
        changed = halfway(harness, client);
        CHECK(harness.compileStats().pipelines == pipelines);
        CHECK(differing(plain, changed) > plain.width * plain.height / 20);
        saved = nlohmann::json(harness.document().document()).get<azl::Composition>();
        CHECK(saved.layers[0].transitionParams == std::map<std::string, Params>{{"windowSlice", {{"count", 2.0}}}});

        client.send(msg::undo());
        const Message undone = client.next();
        CHECK(undone["op"] == "setTransitionParam");
        CHECK(undone["value"].is_null());
        CHECK(harness.document().document().layers[0].transitionParams.empty());
    }
    {
        azl::EngineHarness harness({.width = 160, .height = 90}, 60.0, {}, saved);
        Client client(harness.document());
        client.send({{"type", "trigger"}, {"op", "clip"}, {"layer", 1}, {"column", 1}});
        REQUIRE(client.next()["op"] == "triggerClip");
        harness.step(70);
        CHECK(differing(halfway(harness, client), changed) < 16);
    }
}
