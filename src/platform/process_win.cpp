#include "platform/process.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace azl {

namespace {

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()));
    return out;
}

std::string systemMessage(DWORD code)
{
    char* text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = text ? text : "error " + std::to_string(code);
    LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.'))
        message.pop_back();
    return message;
}

// One argument as the C runtime's command line parser reads it back.
void appendQuoted(std::wstring& line, const std::wstring& arg)
{
    if (!line.empty())
        line += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line += L'"';
    for (auto it = arg.begin();; ++it) {
        size_t slashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++slashes;
        }
        if (it == arg.end()) {
            line.append(slashes * 2, L'\\');
            break;
        }
        line.append(*it == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        line += *it;
    }
    line += L'"';
}

bool sameName(const std::wstring& a, const std::wstring& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](wchar_t x, wchar_t y) { return std::towupper(x) == std::towupper(y); });
}

std::wstring nameOf(const std::wstring& entry)
{
    return entry.substr(0, entry.find(L'=', 1));
}

// This process's environment with the overrides applied, sorted by name as CreateProcess expects, double-NUL ended.
std::wstring environmentFor(const SpawnOptions& options)
{
    std::map<std::string, std::optional<std::string>> overrides = options.environment;
    if (options.heartbeat)
        overrides["NOCTURNE_HEARTBEAT_HANDLE"] = std::to_string(reinterpret_cast<uintptr_t>(*options.heartbeat));
    std::vector<std::wstring> entries;
    wchar_t* block = GetEnvironmentStringsW();
    for (const wchar_t* e = block; *e; e += wcslen(e) + 1) {
        const std::wstring entry(e);
        const bool overridden = std::any_of(overrides.begin(), overrides.end(),
                                            [&](const auto& o) { return sameName(nameOf(entry), wide(o.first)); });
        if (!overridden)
            entries.push_back(entry);
    }
    FreeEnvironmentStringsW(block);
    for (const auto& [name, value] : overrides)
        if (value)
            entries.push_back(wide(name) + L"=" + wide(*value));
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(nameOf(a).c_str(), -1, nameOf(b).c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring out;
    for (const std::wstring& entry : entries)
        out += entry + L'\0';
    return out + L'\0';
}

// Every child that is not detached joins this job, which ends them all when this process ends, however it ends.
HANDLE childrenJob()
{
    static const HANDLE job = [] {
        HANDLE created = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (created)
            SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        return created;
    }();
    return job;
}

// The handles a child gets, made inheritable only for the CreateProcess call that names them in its handle list.
class Inheritance {
public:
    ~Inheritance()
    {
        for (const auto& [handle, flags] : handles_)
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, flags);
        for (HANDLE opened : opened_)
            CloseHandle(opened);
    }

    HANDLE add(HANDLE handle)
    {
        if (!handle || handle == INVALID_HANDLE_VALUE)
            return nullptr;
        if (std::none_of(handles_.begin(), handles_.end(), [&](const auto& h) { return h.first == handle; })) {
            DWORD flags = 0;
            GetHandleInformation(handle, &flags);
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            handles_.emplace_back(handle, flags & HANDLE_FLAG_INHERIT);
        }
        return handle;
    }

    HANDLE stream(const Stdio& stdio, DWORD standard)
    {
        switch (stdio.kind) {
        case Stdio::Kind::Inherit: return add(GetStdHandle(standard));
        case Stdio::Kind::Handle: return add(stdio.handle);
        case Stdio::Kind::Null: break;
        }
        HANDLE null = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (null != INVALID_HANDLE_VALUE)
            opened_.push_back(null);
        return add(null);
    }

    std::vector<HANDLE> list() const
    {
        std::vector<HANDLE> out;
        for (const auto& handle : handles_)
            out.push_back(handle.first);
        return out;
    }

private:
    std::vector<std::pair<HANDLE, DWORD>> handles_;
    std::vector<HANDLE> opened_;
};

} // namespace

std::optional<PipeEnds> makePipe()
{
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, nullptr, 0))
        return std::nullopt;
    return PipeEnds{read, write};
}

long readHandle(NativeHandle handle, void* buffer, size_t size)
{
    DWORD n = 0;
    if (ReadFile(handle, buffer, DWORD(size), &n, nullptr))
        return long(n);
    return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
}

long writeHandle(NativeHandle handle, const void* data, size_t size)
{
    DWORD n = 0;
    return WriteFile(handle, data, DWORD(size), &n, nullptr) ? long(n) : -1;
}

void closeHandle(NativeHandle handle)
{
    CloseHandle(handle);
}

std::optional<Process> spawnProcess(const SpawnOptions& options, std::string* error)
{
    const auto fail = [error](DWORD code) -> std::optional<Process> {
        if (error)
            *error = systemMessage(code);
        return std::nullopt;
    };
    Inheritance inheritance;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inheritance.stream(options.input, STD_INPUT_HANDLE);
    startup.StartupInfo.hStdOutput = inheritance.stream(options.output, STD_OUTPUT_HANDLE);
    startup.StartupInfo.hStdError = inheritance.stream(options.error, STD_ERROR_HANDLE);
    if (options.heartbeat)
        inheritance.add(*options.heartbeat);
    std::vector<HANDLE> inherited = inheritance.list();

    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<char> attributes(attributesSize);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributesSize))
        return fail(GetLastError());
    const bool listed = inherited.empty() ||
                        UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                  inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr);
    if (!listed) {
        const DWORD code = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return fail(code);
    }

    std::wstring commandLine;
    appendQuoted(commandLine, options.program.wstring());
    for (const std::string& arg : options.args)
        appendQuoted(commandLine, wide(arg));
    std::wstring environment = environmentFor(options);
    const std::wstring program = options.program.wstring();
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED;
    if (options.detached)
        flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    else if (!GetConsoleWindow())
        flags |= CREATE_NO_WINDOW;

    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(options.searchPath ? nullptr : program.c_str(), commandLine.data(), nullptr,
                                        nullptr, !inherited.empty(), flags, environment.data(), nullptr,
                                        &startup.StartupInfo, &info);
    const DWORD code = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created)
        return fail(code);
    if (!options.detached)
        AssignProcessToJobObject(childrenJob(), info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    Process process;
    process.pid_ = int(info.dwProcessId);
    process.handle_ = info.hProcess;
    return process;
}

Process::Process(Process&& other) noexcept
    : pid_(std::exchange(other.pid_, 0)), exit_(other.exit_), handle_(std::exchange(other.handle_, nullptr))
{
}

Process& Process::operator=(Process&& other) noexcept
{
    if (handle_)
        CloseHandle(handle_);
    pid_ = std::exchange(other.pid_, 0);
    exit_ = other.exit_;
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
}

Process::~Process()
{
    if (handle_)
        CloseHandle(handle_);
}

std::optional<int> Process::poll()
{
    DWORD code = 0;
    if (!exit_ && handle_ && WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0 && GetExitCodeProcess(handle_, &code))
        exit_ = int(code);
    return exit_;
}

int Process::wait()
{
    if (!exit_ && handle_)
        WaitForSingleObject(handle_, INFINITE);
    return poll().value_or(-1);
}

void Process::terminate()
{
    kill();
}

void Process::kill()
{
    if (!exit_ && handle_)
        TerminateProcess(handle_, 1);
}

std::optional<NativeHandle> inheritedHeartbeat()
{
    const char* value = std::getenv("NOCTURNE_HEARTBEAT_HANDLE");
    if (!value)
        return std::nullopt;
    return reinterpret_cast<NativeHandle>(uintptr_t(std::strtoull(value, nullptr, 10)));
}

} // namespace azl
