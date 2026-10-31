#include "platform/exit_trace.hpp"

#include <windows.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <stdlib.h>

namespace azl {

namespace {

constexpr int kMaxFrames = 32;
// Fatal exceptions reported when raised; a library that keeps faulting and handling it must not flood the log.
constexpr int kMaxRaised = 8;

std::atomic<bool> expected{false};
std::atomic<int> raised{0};
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;

// One line of text, written to stderr in one piece. Never allocates, so it works while the heap is broken.
class Line {
public:
    ~Line()
    {
        text_[n_++] = '\n';
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), text_, DWORD(n_), &written, nullptr);
    }

    Line& operator<<(const char* text)
    {
        while (*text && n_ < kRoom)
            text_[n_++] = *text++;
        return *this;
    }

    Line& hex(uint64_t value)
    {
        char digits[16];
        int count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value & 15];
            value >>= 4;
        } while (value);
        *this << "0x";
        while (count && n_ < kRoom)
            text_[n_++] = digits[--count];
        return *this;
    }

    Line& dec(uint64_t value)
    {
        char digits[20];
        int count = 0;
        do {
            digits[count++] = char('0' + value % 10);
            value /= 10;
        } while (value);
        while (count && n_ < kRoom)
            text_[n_++] = digits[--count];
        return *this;
    }

    // The module's file name and the offset in it, or the bare address outside every module.
    Line& address(uint64_t at)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(at), &module))
            return hex(at);
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        DWORD name = length;
        while (name && path[name - 1] != L'\\')
            --name;
        for (DWORD i = name; i < length && n_ < kRoom; ++i)
            text_[n_++] = path[i] < 128 ? char(path[i]) : '?';
        *this << "+";
        return hex(at - reinterpret_cast<uint64_t>(module));
    }

    Line& thread()
    {
        *this << "thread ";
        dec(GetCurrentThreadId());
        wchar_t* description = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description)) && description && *description) {
            *this << " (";
            for (const wchar_t* c = description; *c && n_ < kRoom; ++c)
                text_[n_++] = *c < 128 ? char(*c) : '?';
            *this << ")";
        }
        LocalFree(description);
        return *this;
    }

private:
    static constexpr size_t kRoom = 511;
    char text_[kRoom + 1];
    size_t n_ = 0;
};

// The calling thread's stack from `context` down, one frame per line, by the unwind data every x64 function has. A
// frame without it (a leaf, or a call through a null pointer) returns to the address on top of its stack.
void writeStack(CONTEXT context)
{
#if defined(_M_X64)
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    for (int i = 0; i < kMaxFrames && (i == 0 || context.Rip); ++i) {
        (Line() << "    ").address(context.Rip);
        DWORD64 base = 0;
        RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
        if (!function) {
            if (context.Rsp < low || context.Rsp + 8 > high)
                return;
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context, &handlerData, &establisher,
                         nullptr);
    }
#else
    (void)context;
#endif
}

void writeStackHere()
{
    CONTEXT context{};
    RtlCaptureContext(&context);
    writeStack(context);
}

// The system's exceptions of error severity: access violations, stack overflows, heap corruption, illegal
// instructions. Not C++ exceptions (0xE06D7363), which carry the customer bit.
bool fatal(DWORD code)
{
    return (code & 0xF0000000u) == 0xC0000000u;
}

void describe(Line& line, const EXCEPTION_RECORD& record)
{
    line << "exception ";
    line.hex(record.ExceptionCode);
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        const ULONG_PTR kind = record.ExceptionInformation[0];
        line << (kind == 0 ? " reading " : kind == 1 ? " writing " : " executing ");
        line.hex(record.ExceptionInformation[1]);
    }
    line << " at ";
    line.address(reinterpret_cast<uint64_t>(record.ExceptionAddress));
}

LONG CALLBACK onRaised(EXCEPTION_POINTERS* info)
{
    if (!fatal(info->ExceptionRecord->ExceptionCode) || raised.fetch_add(1) >= kMaxRaised)
        return EXCEPTION_CONTINUE_SEARCH;
    {
        Line line;
        line << "nocturne: ";
        describe(line, *info->ExceptionRecord);
        line << " on ";
        // A thread out of stack has no room to look its name up.
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
            (line << "thread ").dec(GetCurrentThreadId());
        else
            line.thread();
    }
    writeStack(*info->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI onUnhandled(EXCEPTION_POINTERS* info)
{
    {
        Line line;
        line << "nocturne: unhandled ";
        describe(line, *info->ExceptionRecord);
        line << " on ";
        line.thread();
        line << ": the process ends";
    }
    return previousFilter ? previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// With the exception the thread is handling, if any.
void report(const char* what)
{
    {
        Line line;
        line << "nocturne: " << what << " on ";
        line.thread();
        if (const std::exception_ptr current = std::current_exception()) {
            try {
                std::rethrow_exception(current);
            } catch (const std::exception& e) {
                line << ": " << e.what();
            } catch (...) {
                line << ": an exception that is not a std::exception";
            }
        }
    }
    writeStackHere();
}

// The C runtime keeps a terminate handler per thread, so this one runs on the main thread only; on the others
// std::terminate ends in abort, which every thread reports.
[[noreturn]] void onTerminate()
{
    report("std::terminate");
    std::abort();
}

void onAbort(int)
{
    report("abort()");
}

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
{
    report("invalid parameter to a C runtime function");
    std::abort();
}

void onPureCall()
{
    report("pure virtual function call");
    std::abort();
}

void onExit()
{
    if (!expected)
        report("exit() called");
}

void onQuickExit()
{
    if (!expected)
        report("quick_exit() called");
}

// The executable's own detach comes only when the process ends. The thread that called ExitProcess runs it, every
// other thread is gone already.
void NTAPI onTls(PVOID, DWORD reason, PVOID)
{
    if (reason != DLL_PROCESS_DETACH || expected)
        return;
    report("ExitProcess called");
}

} // namespace

} // namespace azl

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:azlExitTraceTls")
#pragma section(".CRT$XLY", long, read)
extern "C" __declspec(allocate(".CRT$XLY")) const PIMAGE_TLS_CALLBACK azlExitTraceTls = azl::onTls;

namespace azl {

void traceExits()
{
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    AddVectoredExceptionHandler(1, onRaised);
    previousFilter = SetUnhandledExceptionFilter(onUnhandled);
    std::set_terminate(onTerminate);
    std::signal(SIGABRT, onAbort);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_purecall_handler(onPureCall);
    std::atexit(onExit);
    std::at_quick_exit(onQuickExit);
}

void exitingNormally()
{
    expected = true;
}

} // namespace azl
