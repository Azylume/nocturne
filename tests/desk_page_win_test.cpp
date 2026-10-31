// The Windows twin of tests/desk_page_test.mm: the desk page in a real browser engine, Edge without a window, which
// prints the page as its script left it.
#include "desk_rig.hpp"
#include "test_paths.hpp"

#include <cstdlib>
#include <regex>
#include <string>
#include <vector>

namespace {

fs::path edge()
{
    for (const char* root : {"ProgramFiles(x86)", "ProgramFiles"})
        if (const char* folder = std::getenv(root))
            if (const fs::path path = fs::path(folder) / "Microsoft/Edge/Application/msedge.exe"; fs::exists(path))
                return path;
    return {};
}

// The page's DOM once its first fetches have come back, from Edge with no window.
std::string pageDom(uint16_t port)
{
    TempDir profile;
    const fs::path browser = edge();
    REQUIRE_MESSAGE(!browser.empty(), "needs Microsoft Edge");
    const CtlOutput run = runProgram(
        {.program = browser,
         // Started elevated in a desktop session, Edge relaunches itself unelevated and prints nothing here.
         .args = {"--headless", "--do-not-de-elevate", "--disable-gpu", "--no-first-run",
                  "--user-data-dir=" + profile.path.string(),
                  "--virtual-time-budget=3000", "--window-size=390,844", "--dump-dom",
                  "http://127.0.0.1:" + std::to_string(port) + "/"}});
    REQUIRE(run.code == 0);
    return run.output;
}

struct Slot {
    std::string classes;
    std::string dj;
    std::string meta;
};

std::vector<Slot> slots(const std::string& dom)
{
    static const std::regex button(
        R"re(<button class="(slot[^"]*)"[^>]*><span class="dj">([^<]*)</span><span class="meta">([^<]*)</span>)re");
    std::vector<Slot> found;
    for (std::sregex_iterator it(dom.begin(), dom.end(), button), end; it != end; ++it)
        found.push_back({(*it)[1], (*it)[2], (*it)[3]});
    return found;
}

const Slot* withClass(const std::vector<Slot>& slots, const std::string& name)
{
    for (const Slot& slot : slots)
        if ((" " + slot.classes + " ").find(" " + name + " ") != std::string::npos)
            return &slot;
    return nullptr;
}

std::string text(const std::string& dom, const std::string& id)
{
    std::smatch match;
    const std::regex element("id=\"" + id + "\"[^>]*>([^<]*)<");
    return std::regex_search(dom, match, element) ? match[1].str() : std::string();
}

std::string bodyClasses(const std::string& dom)
{
    std::smatch match;
    return std::regex_search(dom, match, std::regex(R"re(<body class="([^"]*)")re")) ? match[1].str() : "";
}

} // namespace

TEST_CASE("desk: the page draws a button per slot and marks the live one, in Edge with no window")
{
    Desk desk;
    const std::string dom = pageDom(desk.server.port());
    INFO(dom);
    const std::vector<Slot> lineup = slots(dom);
    REQUIRE(lineup.size() == 4);
    REQUIRE(withClass(lineup, "live"));
    CHECK(withClass(lineup, "live")->dj == "Opener");
    REQUIRE(withClass(lineup, "next"));
    CHECK(withClass(lineup, "next")->dj == "Headliner");
    REQUIRE(withClass(lineup, "missing"));
    CHECK(withClass(lineup, "missing")->meta.find("deck missing") != std::string::npos);
    CHECK(text(dom, "status").starts_with("Next: Headliner at 03:00"));
    CHECK(bodyClasses(dom).find("offline") == std::string::npos);

    Desk none(azl::defaultComposition());
    const std::string empty = pageDom(none.server.port());
    INFO(empty);
    CHECK(bodyClasses(empty).find("empty") != std::string::npos);
    CHECK(slots(empty).empty());
    CHECK(text(empty, "liveDeck").starts_with("Live: "));
}
