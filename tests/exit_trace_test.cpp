#include <doctest.h>

#include "platform/process.hpp"

#include <cstdio>
#include <optional>
#include <string>

namespace {

struct Ended {
    unsigned exit = 0;
    std::string error;
};

// Runs the test child's `trace HOW` job and collects what it wrote on stderr.
Ended endTraced(const std::string& how)
{
    const std::optional<azl::PipeEnds> pipe = azl::makePipe();
    REQUIRE(pipe);
    std::optional<azl::Process> child = azl::spawnProcess(
        {.program = NOCTURNE_PLATFORM_CHILD, .args = {"trace", how}, .error = azl::Stdio::to(pipe->write)});
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
