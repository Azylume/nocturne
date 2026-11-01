#include "platform/exit_trace.hpp"

#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <stdlib.h>

#pragma comment(lib, "dbghelp.lib")

namespace azl {

namespace {

constexpr int kMaxFrames = 48;
// Fatal exceptions reported when raised; a library that keeps faulting and handling it must not flood the log.
constexpr int kMaxRaised = 8;

// Until traceExits, every exit is expected: a process that links this file but never traces leaves nothing.
std::atomic<bool> expected{true};
std::atomic<int> raised{0};
LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;

// The last fatal exception this thread raised, kept for the exit an unhandled-exception filter that is not ours (the
// test runner's) makes after it.
struct Raised {
    EXCEPTION_RECORD record;
    CONTEXT context;
    bool set = false;
};
thread_local Raised raisedHere;

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
        wide(path + name, length - name);
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
            wide(description, wcslen(description));
            *this << ")";
        }
        LocalFree(description);
        return *this;
    }

    // ASCII as is, every other character as '?'.
    Line& wide(const wchar_t* text, size_t length)
    {
        for (size_t i = 0; i < length && n_ < kRoom; ++i)
            text_[n_++] = text[i] < 128 ? char(text[i]) : '?';
        return *this;
    }

private:
    static constexpr size_t kRoom = 511;
    char text_[kRoom + 1];
    size_t n_ = 0;
};

struct Stack {
    uint64_t frames[kMaxFrames];
    int count = 0;
};

// The calling thread's stack from `context` down, by the unwind data every x64 function has. A frame without it (a
// leaf, or a call through a null pointer) returns to the address on top of its stack.
Stack walk(CONTEXT context)
{
    Stack stack;
#if defined(_M_X64)
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    while (stack.count < kMaxFrames && (stack.count == 0 || context.Rip)) {
        stack.frames[stack.count++] = context.Rip;
        DWORD64 base = 0;
        RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
        if (!function) {
            if (context.Rsp < low || context.Rsp + 8 > high)
                break;
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
    return stack;
}

// Loads the symbols of every module once, from the PDBs next to them, and keeps dbghelp, which is single threaded, to
// one thread. A thread that faults while another one (or itself) looks symbols up goes without. Unlike the rest of
// this file it allocates and reads files, so it runs only after the raw stack is out.
class Symbols {
public:
    Symbols() : owned_(TryAcquireSRWLockExclusive(&lock)) {}
    ~Symbols()
    {
        if (owned_)
            ReleaseSRWLockExclusive(&lock);
    }

    bool ready()
    {
        if (!owned_)
            return false;
        if (!tried) {
            tried = true;
            wchar_t folder[MAX_PATH];
            DWORD length = GetModuleFileNameW(nullptr, folder, MAX_PATH);
            while (length && folder[length - 1] != L'\\')
                --length;
            folder[length] = 0;
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
                          SYMOPT_NO_PROMPTS);
            loaded = SymInitializeW(GetCurrentProcess(), folder, TRUE);
        }
        return loaded;
    }

private:
    const bool owned_;
    static inline SRWLOCK lock = SRWLOCK_INIT;
    static inline bool tried = false;
    static inline bool loaded = false;
};

// The function, and the source line where there is one. A return address points after its call, so its line is the
// one before it.
void writeSymbol(Line& line, uint64_t at, bool returnAddress)
{
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 256;
    DWORD64 offset = 0;
    if (!SymFromAddr(GetCurrentProcess(), at, &offset, symbol))
        return;
    (line << " " << symbol->Name << "+").hex(offset);
    IMAGEHLP_LINE64 source{};
    source.SizeOfStruct = sizeof source;
    DWORD column = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), at - (returnAddress ? 1 : 0), &column, &source))
        (line << " (" << source.FileName << ":").dec(source.LineNumber) << ")";
}

// One frame per line as module+offset first, which needs neither the heap nor files; then, where dbghelp finds them,
// the same frames with function names and lines.
void writeStack(const Stack& stack, bool symbols)
{
    for (int i = 0; i < stack.count; ++i)
        (Line() << "    ").address(stack.frames[i]);
    if (!symbols)
        return;
    Symbols lookup;
    if (!lookup.ready())
        return;
    Line() << "nocturne: the same stack with symbols:";
    for (int i = 0; i < stack.count; ++i) {
        Line line;
        (line << "    ").address(stack.frames[i]);
        writeSymbol(line, stack.frames[i], i > 0);
    }
}

void writeStackHere()
{
    CONTEXT context{};
    RtlCaptureContext(&context);
    writeStack(walk(context), true);
}

// A minidump of the process with `exception`, into NOCTURNE_CRASH_DUMP_DIR if it is set, as <exe>.<pid>.dmp.
void writeDump(EXCEPTION_POINTERS* exception)
{
    wchar_t path[MAX_PATH];
    const DWORD folder = GetEnvironmentVariableW(L"NOCTURNE_CRASH_DUMP_DIR", path, MAX_PATH);
    if (!folder || folder > MAX_PATH - 80)
        return;
    wchar_t module[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    DWORD name = length;
    while (name && module[name - 1] != L'\\')
        --name;
    CreateDirectoryW(path, nullptr);
    std::swprintf(path + folder, MAX_PATH - folder, L"\\%.60ls.%lu.dmp", module + name, GetCurrentProcessId());
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
    const auto type = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                    MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
    const bool written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                           exception ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    Line line;
    line << (written ? "nocturne: wrote a minidump to " : "nocturne: could not write a minidump to ");
    line.wide(path, wcslen(path));
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
    if (!fatal(info->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    raisedHere.record = *info->ExceptionRecord;
    raisedHere.context = *info->ContextRecord;
    raisedHere.set = true;
    if (raised.fetch_add(1) >= kMaxRaised)
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
    // A thread out of stack has no room for dbghelp either.
    writeStack(walk(*info->ContextRecord), info->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW);
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
    writeDump(info);
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

// A filter that ends the process with exit() after a fatal exception on this thread (doctest's) leaves that exception
// and a dump too.
void onExit()
{
    if (expected)
        return;
    report("exit() called");
    if (!raisedHere.set)
        return;
    {
        Line line;
        line << "nocturne: after ";
        describe(line, raisedHere.record);
    }
    writeStack(walk(raisedHere.context), true);
    EXCEPTION_POINTERS pointers{&raisedHere.record, &raisedHere.context};
    writeDump(&pointers);
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
    expected = false;
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
