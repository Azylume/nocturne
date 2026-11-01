#include "audio/audio_input.hpp"

#include <doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cmath>
#include <numbers>
#endif

namespace {

bool realAudio()
{
    const char* value = std::getenv("NOCTURNE_REAL_AUDIO");
    return value && std::string(value) == "1";
}

double seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

azl::AudioInputStatus selectedInput(azl::AudioInput& input, const std::string& device)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool selected = false;
    input.onChange([&](const azl::AudioInputStatus&, const std::string& event) {
        std::lock_guard lock(mutex);
        selected = selected || event.starts_with("select");
        changed.notify_all();
    });
    input.select(device);
    std::unique_lock lock(mutex);
    const bool done = changed.wait_for(lock, std::chrono::seconds(5), [&] { return selected; });
    lock.unlock();
    input.onChange(nullptr);
    REQUIRE(done);
    return input.status();
}

} // namespace

TEST_CASE("an audio input never crashes on a missing device: it falls back to the default input or runs on nothing" *
          doctest::skip(!realAudio()))
{
    azl::AudioAnalysis analysis(seconds);
    azl::AudioInput input(analysis);
    const azl::AudioInputStatus missing = selectedInput(input, "No Such Device 1234");
    CHECK(missing.requested == "No Such Device 1234");
    CHECK((missing.state == "fallback" || missing.state == "none"));
    CHECK(selectedInput(input, "off").state == "off");
    const azl::AudioInputStatus fallback = selectedInput(input, "");
    CHECK((fallback.state == "running" || fallback.state == "none"));
    MESSAGE("default input: " << fallback.state << ", " << fallback.name << " at " << fallback.sampleRate << " Hz");
    for (const azl::AudioDeviceInfo& d : azl::AudioInput::devices()) {
        CHECK(d.channels > 0);
        MESSAGE("input: " << d.name << ", " << d.channels << " channels at " << d.sampleRate << " Hz"
                          << std::string(d.isDefault ? ", default" : ""));
    }
}

TEST_CASE("NOCTURNE_NO_AUDIO_INPUT keeps every audio input off, whatever is selected" * doctest::skip(realAudio()))
{
    REQUIRE(azl::AudioInput::disabled());
    azl::AudioAnalysis analysis(seconds);
    azl::AudioInput input(analysis);
    for (const std::string device : {"", "No Such Device 1234", "system"}) {
        const azl::AudioInputStatus status = selectedInput(input, device);
        CHECK(status.requested == "off");
        CHECK(status.state == "off");
        CHECK(status.uid.empty());
    }
}

#ifdef _WIN32

namespace {

using Microsoft::WRL::ComPtr;

constexpr double kToneHz = 1000.0;
constexpr const char* kToneSeconds = "NOCTURNE_TONE_SECONDS";

// Plays a 1 kHz tone at -12 dBFS on the default output for `duration` seconds.
void playTone(double duration)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> devices;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEX* format = nullptr;
    REQUIRE(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))));
    REQUIRE(SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)));
    REQUIRE(SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)));
    REQUIRE(SUCCEEDED(client->GetMixFormat(&format)));
    REQUIRE(format->wBitsPerSample == 32);
    REQUIRE(SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr)));
    REQUIRE(SUCCEEDED(client->GetService(IID_PPV_ARGS(&render))));
    UINT32 size = 0;
    client->GetBufferSize(&size);
    client->Start();
    const double rate = format->nSamplesPerSec;
    uint64_t written = 0;
    for (const double end = seconds() + duration; seconds() < end;) {
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        BYTE* data = nullptr;
        if (const UINT32 frames = size - padding; frames > 0 && SUCCEEDED(render->GetBuffer(frames, &data))) {
            auto* out = reinterpret_cast<float*>(data);
            for (UINT32 i = 0; i < frames; ++i, ++written) {
                const float x = float(0.25 * std::sin(2.0 * std::numbers::pi * kToneHz * double(written) / rate));
                for (WORD c = 0; c < format->nChannels; ++c)
                    out[size_t(i) * format->nChannels + c] = x;
            }
            render->ReleaseBuffer(frames, 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client->Stop();
    CoTaskMemFree(format);
}

// This test binary in another process, playing the tone. Its parent is the shell, not this process: system audio
// leaves out the capturing process's own tree.
struct TonePlayer {
    PROCESS_INFORMATION process{};

    explicit TonePlayer(double duration)
    {
        std::array<wchar_t, MAX_PATH> path{};
        GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        std::wstring command = L"\"" + std::wstring(path.data()) + L"\" --test-case=\"audio input: a tone*\"";
        DWORD shellPid = 0;
        GetWindowThreadProcessId(GetShellWindow(), &shellPid);
        HANDLE shell = OpenProcess(PROCESS_CREATE_PROCESS, FALSE, shellPid);
        REQUIRE(shell);
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<uint8_t> storage(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        InitializeProcThreadAttributeList(attributes, 1, 0, &size);
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &shell, sizeof shell, nullptr,
                                  nullptr);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof startup;
        startup.lpAttributeList = attributes;
        SetEnvironmentVariableA(kToneSeconds, std::to_string(duration).c_str());
        const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                            &startup.StartupInfo, &process);
        SetEnvironmentVariableA(kToneSeconds, nullptr);
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(shell);
        REQUIRE(created);
    }
    ~TonePlayer()
    {
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
};

// The loudest level the analysis reads within `within` seconds.
float loudest(azl::AudioAnalysis& analysis, double within)
{
    float level = 0.0f;
    for (const double end = seconds() + within; seconds() < end;) {
        const azl::AudioFrame frame = analysis.latest();
        if (frame.live)
            level = std::max(level, frame.values[size_t(azl::AudioFeature::Mid)]);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return level;
}

} // namespace

TEST_CASE("audio input: a tone for the loopback cases, played by a child process" *
          doctest::skip(!std::getenv(kToneSeconds)))
{
    playTone(std::stod(std::getenv(kToneSeconds)));
}

TEST_CASE("audio input: system audio and one app's audio come in through process loopback" *
          doctest::skip(!realAudio()))
{
    azl::AudioAnalysis analysis(seconds);
    azl::AudioInput input(analysis);
    TonePlayer player(6.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const azl::AudioInputStatus system = selectedInput(input, "system");
    CHECK(system.state == "running");
    CHECK(system.uid == "system");
    CHECK(system.name == "system audio");
    CHECK(system.sampleRate > 0.0);
    const float systemLevel = loudest(analysis, 1.5);
    MESSAGE("system audio: mid band " << systemLevel);
    CHECK(systemLevel > 0.5f);

    const auto apps = azl::AudioInput::apps();
    CHECK(std::any_of(apps.begin(), apps.end(), [&](const azl::AudioAppInfo& a) {
        return a.pid == int(player.process.dwProcessId) && a.playing;
    }));
    const azl::AudioInputStatus app = selectedInput(input, "app:" + std::to_string(player.process.dwProcessId));
    CHECK(app.state == "running");
    CHECK(app.uid == "app");
    CHECK(app.name.starts_with("app audio ("));
    const float appLevel = loudest(analysis, 1.5);
    MESSAGE(app.name << ": mid band " << appLevel);
    CHECK(appLevel > 0.5f);
}

TEST_CASE("audio input: the default input delivers samples and channel levels" * doctest::skip(!realAudio()))
{
    azl::AudioAnalysis analysis(seconds);
    azl::AudioInput input(analysis);
    const azl::AudioInputStatus status = selectedInput(input, "");
    REQUIRE(status.state == "running");
    bool delivered = false;
    for (const double end = seconds() + 2.0; seconds() < end && !delivered;) {
        delivered = analysis.latest().live && !analysis.channels().empty();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(delivered);
    for (const azl::ChannelLevel& level : analysis.channels())
        MESSAGE("channel level " << level.levelDb << " dB, noise " << level.noise);
}

TEST_CASE("audio input: an app that is not running falls back to the default input" * doctest::skip(!realAudio()))
{
    azl::AudioAnalysis analysis(seconds);
    azl::AudioInput input(analysis);
    const azl::AudioInputStatus status = selectedInput(input, "app:no-such-app-1234");
    CHECK(status.requested == "app:no-such-app-1234");
    CHECK((status.state == "fallback" || status.state == "none"));
}

#endif
