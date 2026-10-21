#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <doctest.h>

#include <sys/wait.h>

#include <functional>

namespace {

struct Run {
    std::string out;
    int status = 0;
};

Run ctl(uint16_t port, const std::string& args)
{
    const fs::path binary = ctlPath();
    const std::string command = "'" + binary.string() + "' --port " + std::to_string(port) + " " + args + " 2>&1";
    Run run;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe);
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe))
        run.out += buffer;
    const int wait = pclose(pipe);
    run.status = WEXITSTATUS(wait);
    return run;
}

const std::vector<azl::Point> kSquare{{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};

} // namespace

TEST_CASE("ctl rename: every kind renames through nocturne-ctl, shows in the snapshot and undoes in one step")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    azl::WebSocketServer server(harness.document(), 0);
    const uint16_t port = server.port();
    Client client(harness.document());
    apply(client, azl::AddScreen{std::nullopt,
                                 {.name = "Left",
                                  .slices = {azl::Slice{.masks = {{.points = kSquare}}}},
                                  .masks = {{.points = kSquare}}}});
    CHECK(ctl(port, "macro add Wash").status == 0);

    auto document = [&] { return harness.document().document(); };
    const azl::Composition doc = document();
    const azl::Screen& screen = doc.screens.back();
    REQUIRE(!doc.macros.empty());
    const uint64_t deck = doc.decks[0].id, column = doc.decks[0].columns[0].id, layer = doc.layers[0].id;
    const uint64_t macro = doc.macros.back().id, slice = screen.slices[0].id;
    const uint64_t sliceMask = screen.slices[0].masks[0].id, screenMask = screen.masks[0].id;

    struct Case {
        std::string kind;
        uint64_t id;
        std::function<std::string()> name;
    };
    const std::vector<Case> cases{
        {"deck", deck, [&] { return document().decks[0].name; }},
        {"layer", layer, [&] { return document().layers[0].name; }},
        {"column", column, [&] { return document().decks[0].columns[0].name; }},
        {"screen", screen.id, [&] { return document().screens.back().name; }},
        {"slice", slice, [&] { return document().screens.back().slices[0].name; }},
        {"mask", sliceMask, [&] { return document().screens.back().slices[0].masks[0].name; }},
        {"mask", screenMask, [&] { return document().screens.back().masks[0].name; }},
        {"macro", macro, [&] { return document().macros.back().name; }},
    };
    for (const Case& c : cases) {
        CAPTURE(c.kind);
        const std::string before = c.name();
        const std::string arg = c.kind + " " + std::to_string(c.id);
        CHECK(ctl(port, "rename " + arg + " Second set").status == 0);
        CHECK(c.name() == "Second set");
        CHECK(ctl(port, "rename " + arg + " 'Quoted  name'").status == 0);
        CHECK(c.name() == "Quoted  name");
        CHECK(ctl(port, "undo").status == 0);
        CHECK(c.name() == "Second set");
        CHECK(ctl(port, "undo").status == 0);
        CHECK(c.name() == before);
    }

    CHECK(ctl(port, "rename deck " + std::to_string(deck) + " Seen").status == 0);
    CHECK(ctl(port, "--json snapshot").out.find("\"Seen\"") != std::string::npos);
}

TEST_CASE("ctl rename: an unknown kind or id, a bad id and a missing name fail with a clear message")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    azl::WebSocketServer server(harness.document(), 0);
    const uint16_t port = server.port();

    const Run kind = ctl(port, "rename clip 1 X");
    CHECK(kind.status != 0);
    CHECK(kind.out.find("unknown kind clip") != std::string::npos);
    const Run id = ctl(port, "rename deck 9999 X");
    CHECK(id.status != 0);
    CHECK(id.out.find("no deck 9999") != std::string::npos);
    const Run text = ctl(port, "rename deck two X");
    CHECK(text.status != 0);
    CHECK(text.out.find("not an id") != std::string::npos);
    CHECK(ctl(port, "rename deck 1").status != 0);
}
