#include <doctest.h>

#include "platform/autostart.hpp"
#include "platform/run_command.hpp"
#include "scoped_env.hpp"
#include "temp_folder.hpp"

#ifdef __APPLE__
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef __APPLE__

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& file)
{
    std::ifstream in(file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void writeFile(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

} // namespace

TEST_CASE("platform: runCommand captures a program's output and exit code, and kills one that outlasts its time")
{
    const azl::CommandResult echo = azl::runCommand("/bin/echo", {"one", "two"}, false);
    CHECK(echo.ended);
    CHECK(echo.exitCode == 0);
    CHECK(echo.output == "one two\n");

    const azl::CommandResult fails = azl::runCommand("/usr/bin/false", {}, false);
    CHECK(fails.ended);
    CHECK(fails.exitCode == 1);

    const azl::CommandResult slow = azl::runCommand("/bin/sleep", {"30"}, false, 0.3);
    CHECK_FALSE(slow.ended);
    CHECK(slow.output.find("did not end") != std::string::npos);

    const azl::CommandResult missing = azl::runCommand("/nonexistent/program", {}, false);
    CHECK_FALSE(missing.ended);
}

namespace {

// A home folder with a stand-in launchctl that records its arguments; the real launchd is never asked.
struct StubLaunchctl {
    StubLaunchctl()
        : home("HOME", root.path.string().c_str()),
          program("NOCTURNE_LAUNCHCTL", (root.path / "launchctl").string().c_str())
    {
        writeFile(root.path / "launchctl", "#!/bin/sh\necho \"$@\" >> \"" + (root.path / "calls").string() +
                                               "\"\n"
                                               "[ -f \"" +
                                               (root.path / "fail").string() +
                                               "\" ] && { echo no right; exit 1; }\n"
                                               "[ \"$1\" = print-disabled ] && cat \"" +
                                               (root.path / "disabled").string() + "\"\nexit 0\n");
        fs::permissions(root.path / "launchctl", fs::perms::owner_all);
    }
    void install() { writeFile(root.path / "Library/LaunchAgents/ai.azylume.nocturne.plist", "<plist/>"); }
    void disabled(const std::string& text) { writeFile(root.path / "disabled", text); }
    std::string calls() const { return readFile(root.path / "calls"); }

    TempFolder root;
    ScopedEnv home;
    ScopedEnv program;
};

} // namespace

TEST_CASE("platform: autostart is unsupported until the LaunchAgent is installed, and asks launchd about nothing")
{
    StubLaunchctl stub;
    CHECK(azl::autostartState() == azl::AutostartState::Unsupported);
    const std::optional<std::string> refused = azl::setAutostart(false);
    REQUIRE(refused);
    CHECK(refused->find("not installed") != std::string::npos);
    CHECK(stub.calls().empty());
}

TEST_CASE("platform: autostart reads the job's enabled state from launchctl and toggles only that job")
{
    StubLaunchctl stub;
    stub.install();

    stub.disabled("disabled services = {\n\t\"com.apple.other\" => disabled\n}\n");
    CHECK(azl::autostartState() == azl::AutostartState::On);

    stub.disabled(
        "disabled services = {\n\t\"ai.azylume.nocturne\" => disabled\n\t\"com.apple.other\" => enabled\n}\n");
    CHECK(azl::autostartState() == azl::AutostartState::Off);

    stub.disabled("disabled services = {\n\t\"ai.azylume.nocturne\" => enabled\n}\n");
    CHECK(azl::autostartState() == azl::AutostartState::On);

    CHECK_FALSE(azl::setAutostart(false));
    CHECK_FALSE(azl::setAutostart(true));
    const std::string calls = stub.calls();
    CHECK(calls.find("disable gui/" + std::to_string(::getuid()) + "/ai.azylume.nocturne\n") != std::string::npos);
    CHECK(calls.find("enable gui/" + std::to_string(::getuid()) + "/ai.azylume.nocturne\n") != std::string::npos);
    CHECK(calls.find("bootout") == std::string::npos);
    CHECK(calls.find("bootstrap") == std::string::npos);
}

TEST_CASE("platform: a launchctl that fails gives its message back and leaves the state unsupported")
{
    StubLaunchctl stub;
    stub.install();
    writeFile(stub.root.path / "fail", "");
    const std::optional<std::string> refused = azl::setAutostart(false);
    REQUIRE(refused);
    CHECK(refused->find("no right") != std::string::npos);
    CHECK(azl::autostartState() == azl::AutostartState::Unsupported);
}

#endif
