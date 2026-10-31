#pragma once

#include <doctest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace {

// Ends a process from outside, as a user or the system would.
[[maybe_unused]] void killProcess(int pid)
{
#ifdef _WIN32
    const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid));
    REQUIRE(process);
    TerminateProcess(process, 1);
    CloseHandle(process);
#else
    REQUIRE(::kill(pid, SIGKILL) == 0);
#endif
}

// Freezes every thread of a process, as SIGSTOP does: it neither runs nor ends.
[[maybe_unused]] void suspendProcess(int pid)
{
#ifdef _WIN32
    using Suspend = LONG(NTAPI*)(HANDLE);
    const auto suspend = reinterpret_cast<Suspend>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess")));
    REQUIRE(suspend);
    const HANDLE process = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, DWORD(pid));
    REQUIRE(process);
    CHECK(suspend(process) == 0);
    CloseHandle(process);
#else
    REQUIRE(::kill(pid, SIGSTOP) == 0);
#endif
}

// How the supervisor logs a process that killProcess ended.
#ifdef _WIN32
[[maybe_unused]] constexpr const char* kKilled = "exited with status 1";
#else
[[maybe_unused]] constexpr const char* kKilled = "killed by SIGKILL";
#endif

} // namespace
