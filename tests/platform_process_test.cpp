#include <doctest.h>

#include "platform/process.hpp"
#include "scoped_env.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#endif

namespace {

std::string readAll(azl::NativeHandle handle)
{
    std::string out;
    char buffer[4096];
    for (long n; (n = azl::readHandle(handle, buffer, sizeof buffer)) > 0;)
        out.append(buffer, size_t(n));
    return out;
}

struct Run {
    int exit = -100;
    std::string output;
};

// Runs the test child with the given job, its standard output on a pipe.
Run runChild(std::vector<std::string> args, azl::SpawnOptions options = {})
{
    const std::optional<azl::PipeEnds> pipe = azl::makePipe();
    REQUIRE(pipe);
    options.program = NOCTURNE_PLATFORM_CHILD;
    options.args = std::move(args);
    options.output = azl::Stdio::to(pipe->write);
    std::string error;
    std::optional<azl::Process> child = azl::spawnProcess(options, &error);
    azl::closeHandle(pipe->write);
    REQUIRE_MESSAGE(child, error);
    Run run;
    run.output = readAll(pipe->read);
    azl::closeHandle(pipe->read);
    run.exit = child->wait();
    return run;
}

} // namespace

TEST_CASE("platform: a spawned child's exit code, arguments and output come back")
{
    CHECK(runChild({"exit", "7"}).exit == 7);
    const Run printed = runChild({"print", "plain", "with space", "a \"quoted\" word", "trailing\\", ""},
                                 {.error = azl::Stdio::null()});
    CHECK(printed.exit == 0);
    CHECK(printed.output == "[plain][with space][a \"quoted\" word][trailing\\][]");
}

TEST_CASE("platform: a spawned child's error stream can share its output pipe, and its input can be empty")
{
    const std::optional<azl::PipeEnds> pipe = azl::makePipe();
    REQUIRE(pipe);
    std::optional<azl::Process> child =
        azl::spawnProcess({.program = NOCTURNE_PLATFORM_CHILD, .args = {"print", "x"},
                           .output = azl::Stdio::to(pipe->write), .error = azl::Stdio::to(pipe->write)});
    azl::closeHandle(pipe->write);
    REQUIRE(child);
    CHECK(readAll(pipe->read) == "[x]on stderr");
    azl::closeHandle(pipe->read);
    CHECK(child->wait() == 0);

    CHECK(runChild({"stdin"}, {.input = azl::Stdio::null()}).output == "0");
}

TEST_CASE("platform: a spawned child sees the environment with the overrides applied")
{
    ScopedEnv inherited("NOCTURNE_PLATFORM_TEST", "here");
    CHECK(runChild({"env", "NOCTURNE_PLATFORM_TEST"}).output == "here");
    CHECK(runChild({"env", "NOCTURNE_PLATFORM_TEST"}, {.environment = {{"NOCTURNE_PLATFORM_TEST", "there"}}}).output ==
          "there");
    CHECK(runChild({"env", "NOCTURNE_PLATFORM_TEST"}, {.environment = {{"NOCTURNE_PLATFORM_TEST", std::nullopt}}})
              .output == "<unset>");
}

TEST_CASE("platform: a spawned child writes its heartbeat to the handle it was given")
{
    const std::optional<azl::PipeEnds> beats = azl::makePipe();
    REQUIRE(beats);
    const Run run = runChild({"beat"}, {.heartbeat = beats->write});
    azl::closeHandle(beats->write);
    CHECK(run.exit == 0);
    CHECK(readAll(beats->read) == "beat");
    azl::closeHandle(beats->read);
    CHECK(runChild({"beat"}).exit == 3);
}

TEST_CASE("platform: a spawned child holds no handle it was not given, even an inheritable one")
{
    const std::optional<azl::PipeEnds> pipe = azl::makePipe();
    REQUIRE(pipe);
#ifdef _WIN32
    SetHandleInformation(pipe->write, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
#else
    fcntl(pipe->write, F_SETFD, 0);
#endif
    std::optional<azl::Process> child = azl::spawnProcess({.program = NOCTURNE_PLATFORM_CHILD, .args = {"sleep"}});
    REQUIRE(child);
    azl::closeHandle(pipe->write);
    char byte;
    CHECK(azl::readHandle(pipe->read, &byte, 1) == 0);
    azl::closeHandle(pipe->read);
    child->kill();
    child->wait();
}

TEST_CASE("platform: a spawned child can be polled and killed")
{
    std::optional<azl::Process> child = azl::spawnProcess({.program = NOCTURNE_PLATFORM_CHILD, .args = {"sleep"}});
    REQUIRE(child);
    CHECK(child->pid() > 0);
    CHECK_FALSE(child->poll());
    child->kill();
    CHECK(child->wait() != 0);
    CHECK(child->poll());
}

TEST_CASE("platform: a program is found on PATH, and a missing one reports why it did not start")
{
#ifdef _WIN32
    const azl::SpawnOptions shell{.program = "cmd", .args = {"/c", "exit 3"}, .searchPath = true};
#else
    const azl::SpawnOptions shell{.program = "sh", .args = {"-c", "exit 3"}, .searchPath = true};
#endif
    std::optional<azl::Process> found = azl::spawnProcess(shell);
    REQUIRE(found);
    CHECK(found->wait() == 3);

    std::string error;
    CHECK_FALSE(azl::spawnProcess({.program = std::filesystem::path(NOCTURNE_PLATFORM_CHILD).parent_path() / "missing"},
                                  &error));
    CHECK_FALSE(error.empty());
}
