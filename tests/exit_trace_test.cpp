#include <doctest.h>

#include "platform/process.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace {

struct Ended {
    unsigned exit = 0;
    std::string error;
};

// Runs the test child's `trace HOW` job and collects what it wrote on stderr.
Ended endTraced(const std::string& how, std::map<std::string, std::optional<std::string>> environment = {})
{
    const std::optional<azl::PipeEnds> pipe = azl::makePipe();
    REQUIRE(pipe);
    std::optional<azl::Process> child = azl::spawnProcess(
        {.program = NOCTURNE_PLATFORM_CHILD, .args = {"trace", how}, .error = azl::Stdio::to(pipe->write),
         .environment = std::move(environment)});
    azl::closeHandle(pipe->write);
    REQUIRE(child);
    Ended ended;
    char buffer[4096];
    for (long n; (n = azl::readHandle(pipe->read, buffer, sizeof buffer)) > 0;)
        ended.error.append(buffer, size_t(n));
    azl::closeHandle(pipe->read);
    ended.exit = unsigned(child->wait());
    char code[16];
    std::snprintf(code, sizeof code, "0x%08X", ended.exit);
    MESSAGE(how << " ended with " << code << ":\n" << ended.error);
    return ended;
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

// A frame of the child's own code, so the stack reached the place the process ended from.
const char* const kChildFrame = "    nocturne_platform_child.exe+0x";

} // namespace

TEST_CASE("exit trace: a fault on any thread leaves the exception, its thread and its stack")
{
    for (const char* how : {"fault", "fault-thread"}) {
        const Ended ended = endTraced(how);
        CHECK(ended.exit == 0xC0000005u);
        CHECK(has(ended.error, "nocturne: exception 0xc0000005 writing 0x0 at nocturne_platform_child.exe+0x"));
        CHECK(has(ended.error, "nocturne: unhandled exception 0xc0000005 writing 0x0 at nocturne_platform_child.exe+"));
        CHECK(has(ended.error, kChildFrame));
    }
}

TEST_CASE("exit trace: terminate, abort, pure calls and invalid parameters leave their cause and stack")
{
    const Ended thrown = endTraced("throw-thread");
    CHECK(thrown.exit != 0);
    CHECK(has(thrown.error, "nocturne: abort() on thread "));
    const Ended aborted = endTraced("abort");
    CHECK(aborted.exit != 0);
    CHECK(has(aborted.error, "nocturne: abort() on thread "));
    CHECK(has(aborted.error, kChildFrame));
    const Ended pure = endTraced("pure-call");
    CHECK(pure.exit != 0);
    CHECK(has(pure.error, "nocturne: pure virtual function call on thread "));
    const Ended invalid = endTraced("invalid-parameter");
    CHECK(invalid.exit != 0);
    CHECK(has(invalid.error, "nocturne: invalid parameter to a C runtime function on thread "));
    CHECK(has(invalid.error, kChildFrame));
}

TEST_CASE("exit trace: exit and ExitProcess from anywhere but main's return leave their caller")
{
    const Ended exited = endTraced("exit");
    CHECK(exited.exit == 5);
    CHECK(has(exited.error, "nocturne: exit() called on thread "));
    CHECK(has(exited.error, kChildFrame));
    const Ended process = endTraced("exit-process");
    CHECK(process.exit == 6);
    CHECK(has(process.error, "nocturne: ExitProcess called on thread "));
    CHECK(has(process.error, kChildFrame));
    const Ended normal = endTraced("return");
    CHECK(normal.exit == 0);
    CHECK(normal.error.empty());
}

TEST_CASE("exit trace: a fault a filter ends with exit() leaves the exception, a named stack and a minidump")
{
    const std::filesystem::path dumps = std::filesystem::temp_directory_path() / "nocturne-exit-trace-dumps";
    std::filesystem::remove_all(dumps);
    const Ended ended = endTraced("fault-filtered", {{"NOCTURNE_CRASH_DUMP_DIR", dumps.string()}});
    CHECK(ended.exit == 3);
    CHECK(has(ended.error, "nocturne: after exception 0xc0000005 writing 0x0 at nocturne_platform_child.exe+0x"));
    CHECK(has(ended.error, "nocturne: the same stack with symbols:"));
    CHECK(has(ended.error, "platform_child.cpp:"));
    CHECK(has(ended.error, "nocturne: wrote a minidump to "));
    uintmax_t bytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dumps))
        if (entry.path().extension() == ".dmp")
            bytes += entry.file_size();
    CHECK(bytes > 0);
    std::filesystem::remove_all(dumps);
}

// Crashes this test binary itself, under doctest's own handler, to see by hand what a crash in the suite leaves: the
// stack with names on stderr and a minidump in NOCTURNE_CRASH_DUMP_DIR. Skipped, and it needs NOCTURNE_TEST_CRASH=1.
TEST_CASE("exit trace: NOCTURNE_TEST_CRASH=1 crashes the test binary itself" * doctest::skip())
{
    if (!std::getenv("NOCTURNE_TEST_CRASH"))
        return;
    int* volatile nowhere = nullptr;
    *nowhere = 1;
}
