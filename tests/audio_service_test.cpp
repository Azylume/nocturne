#include "audio/audio_service.hpp"
#include "engine/render_thread.hpp"
#include "techno_loop.hpp"
#include "test_client.hpp"

#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#elif defined(_WIN32)
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <thread>

namespace {

struct AudioRig {
    AudioRig(const std::string& log = {})
        : analysis([] { return 10.0; }, {.thread = false}),
          service(document, analysis, nullptr, [] { return 10.0; }, log)
    {
        const std::vector<float> samples = TechnoLoop{.bars = 1}.render();
        analysis.push(samples.data(), 4096, 9.9, TechnoLoop::kRate);
        analysis.pump();
    }

    azl::DocumentServer document{azl::defaultComposition(), [](auto) {}};
    azl::AudioAnalysis analysis;
    azl::AudioService service;
};

Message audio(Client& client, Message request)
{
    request["type"] = "audio";
    client.send(request);
    return client.next();
}

} // namespace

TEST_CASE("audio state is queryable over the protocol")
{
    AudioRig rig;
    Client client(rig.document);
    const Message state = audio(client, {{"op", "state"}});
    REQUIRE(state["op"] == "state");
    CHECK(state["live"] == true);
    CHECK(state["values"]["level"].get<double>() > 0.0);
    CHECK(state["values"].size() == azl::kAudioFeatures);
    CHECK(state["onsets"].contains("kick"));
    CHECK(state["spectrum"].size() == azl::kSpectrumBands);
    CHECK(audio(client, {{"op", "devices"}})["type"] == "error");
    CHECK(audio(client, {{"op", "frobnicate"}})["type"] == "error");
}

TEST_CASE("audio settings are validated and broadcast to every client")
{
    AudioRig rig;
    Client a(rig.document);
    Client b(rig.document);
    const Message changed = audio(a, {{"op", "settings"}, {"latencyMs", 40}, {"autoGain", false}, {"gainDb", 6},
                                      {"envelopes", {{"kick", {{"releaseMs", 300}}}}}});
    REQUIRE(changed["op"] == "settings");
    CHECK(b.next() == changed);
    CHECK(changed["latencyMs"] == 40.0);
    CHECK(changed["autoGain"] == false);
    CHECK(changed["envelopes"]["kick"]["releaseMs"] == 300.0);
    CHECK(changed["envelopes"]["kick"]["attackMs"] == 0.0);
    CHECK(rig.analysis.settings().latencyMs == 40.0);

    CHECK(audio(a, {{"op", "settings"}, {"latencyMs", 5000}})["type"] == "error");
    CHECK(audio(a, {{"op", "settings"}, {"envelopes", {{"wobble", {{"releaseMs", 3}}}}}})["type"] == "error");
    CHECK(rig.analysis.settings().latencyMs == 40.0);
}

TEST_CASE("a subscribed client gets state messages at its rate until it unsubscribes or leaves")
{
    AudioRig rig;
    Client client(rig.document);
    CHECK(audio(client, {{"op", "subscribe"}, {"rate", 20}})["op"] == "subscribed");
    // Late wakes skip messages instead of bursting: the total bounds the rate from above, the median gap from below.
    const auto start = std::chrono::steady_clock::now();
    std::vector<double> gaps;
    auto last = start;
    for (int i = 0; i < 10; ++i) {
        CHECK(client.next()["op"] == "state");
        const auto now = std::chrono::steady_clock::now();
        if (i > 0)
            gaps.push_back(std::chrono::duration<double>(now - last).count());
        last = now;
    }
    const double seconds = std::chrono::duration<double>(last - start).count();
    std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
    const double median = gaps[gaps.size() / 2];
    MESSAGE("10 state messages in " << seconds << " s, median gap " << median * 1000.0 << " ms");
    CHECK(seconds > 0.35);
    CHECK(median < 0.075);
    CHECK(seconds < 5.0);
    client.send({{"type", "audio"}, {"op", "subscribe"}, {"rate", 500}});
    Message refused = client.next();
    while (refused.value("op", "") == "state")
        refused = client.next();
    CHECK(refused["type"] == "error");

    {
        Client leaving(rig.document);
        audio(leaving, {{"op", "subscribe"}, {"rate", 60}});
    }
    client.send({{"type", "audio"}, {"op", "unsubscribe"}});
    for (Message m = client.next(); m["op"] != "unsubscribed"; m = client.next())
        CHECK(m["op"] == "state");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(audio(client, {{"op", "state"}})["op"] == "state");
}

TEST_CASE("the audio log appends the analysis and settings changes as JSON lines")
{
    TempDir dir;
    const fs::path file = dir.path / "audio.jsonl";
    {
        AudioRig rig(file.string());
        Client client(rig.document);
        audio(client, {{"op", "settings"}, {"latencyMs", 25}});
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }
    std::ifstream in(file);
    std::vector<Message> lines;
    for (std::string line; std::getline(in, line);)
        lines.push_back(Message::parse(line));
    REQUIRE(lines.size() >= 4);
    CHECK(lines.front()["event"] == "start");
    const auto states = std::count_if(lines.begin(), lines.end(), [](const Message& m) { return m.contains("values"); });
    CHECK(states >= 3);
    CHECK(std::any_of(lines.begin(), lines.end(), [](const Message& m) { return m.value("event", "") == "settings"; }));
    for (const Message& m : lines) {
        CHECK(m.contains("wall"));
        CHECK_FALSE(m.contains("spectrum"));
    }
}

#ifdef __APPLE__
// A private aggregate device, which only CoreAudio can make, stands in for a microphone.
TEST_CASE("every client gets the device list again when an audio device is added or removed")
{
    azl::AudioAnalysis analysis([] { return azl::engineTime(); });
    azl::AudioInput input(analysis);
    azl::DocumentServer document{azl::defaultComposition(), [](auto) {}};
    azl::AudioService service(document, analysis, &input, azl::engineTime);
    Client a(document);
    Client b(document);
    const auto pushed = [](Client& client) {
        for (Message m = client.next();; m = client.next())
            if (m["op"] == "devices")
                return m;
    };

    // A private aggregate device stands in for a microphone plugged in and pulled out.
    const void* keys[] = {CFSTR(kAudioAggregateDeviceNameKey), CFSTR(kAudioAggregateDeviceUIDKey),
                          CFSTR(kAudioAggregateDeviceIsPrivateKey)};
    const void* values[] = {CFSTR("Nocturne Test"), CFSTR("nocturne-test-hotplug"), kCFBooleanTrue};
    CFDictionaryRef description = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
    AudioObjectID device = 0;
    REQUIRE(AudioHardwareCreateAggregateDevice(description, &device) == noErr);
    CFRelease(description);
    const Message added = pushed(a);
    CHECK(added["type"] == "audio");
    CHECK(added["devices"].is_array());
    CHECK(pushed(b) == added);

    REQUIRE(AudioHardwareDestroyAggregateDevice(device) == noErr);
    CHECK(pushed(a)["devices"].is_array());
    CHECK(pushed(b)["op"] == "devices");
}
#elif defined(_WIN32)
namespace {

// The interface Sound settings enables and disables endpoints with. Windows does not document it; its layout has been
// the same since Windows 7.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
constexpr CLSID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

// The id of the first capture endpoint disabled in Sound settings (Stereo Mix, say); empty when there is none.
std::string disabledCaptureEndpoint()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> endpoints;
    ComPtr<IMMDevice> endpoint;
    UINT count = 0;
    LPWSTR id = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_DISABLED, &endpoints)) ||
        FAILED(endpoints->GetCount(&count)) || count == 0 || FAILED(endpoints->Item(0, &endpoint)) ||
        FAILED(endpoint->GetId(&id)))
        return {};
    // Endpoint ids are ASCII.
    const std::string result(id, id + wcslen(id));
    CoTaskMemFree(id);
    return result;
}

// Enables a disabled endpoint as Sound settings does, which Windows reports as a device arriving, and disables it
// again when it goes, so the machine is left as it was.
class Enabled {
public:
    explicit Enabled(const std::string& id) : id_(id.begin(), id.end())
    {
        REQUIRE(SUCCEEDED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy_))));
        REQUIRE(SUCCEEDED(policy_->SetEndpointVisibility(id_.c_str(), TRUE)));
    }
    ~Enabled() { disable(); }
    void disable()
    {
        if (policy_)
            CHECK(SUCCEEDED(policy_->SetEndpointVisibility(id_.c_str(), FALSE)));
        policy_.Reset();
    }

private:
    std::wstring id_;
    ComPtr<IPolicyConfig> policy_;
};

} // namespace

// Enabling a disabled capture endpoint and disabling it again stands in for a microphone plugged in and pulled out:
// Windows raises the same device state notification for both.
TEST_CASE("every client gets the device list again when an audio device is added or removed")
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::string endpoint = disabledCaptureEndpoint();
    if (endpoint.empty()) {
        MESSAGE("skipped: needs a capture endpoint disabled in Sound settings");
        CoUninitialize();
        return;
    }
    azl::AudioAnalysis analysis([] { return azl::engineTime(); });
    azl::AudioInput input(analysis);
    azl::DocumentServer document{azl::defaultComposition(), [](auto) {}};
    azl::AudioService service(document, analysis, &input, azl::engineTime);
    Client a(document);
    Client b(document);
    const auto listed = [&endpoint](const Message& devices) {
        return std::any_of(devices.begin(), devices.end(), [&](const Message& d) { return d["uid"] == endpoint; });
    };
    // One change can raise more than one notification, so wait for the list that shows it.
    const auto pushed = [&listed](Client& client, bool present) {
        for (Message m = client.next();; m = client.next())
            if (m["op"] == "devices" && listed(m["devices"]) == present)
                return m;
    };
    {
        Enabled enabled(endpoint);
        const Message added = pushed(a, true);
        CHECK(added["type"] == "audio");
        CHECK(pushed(b, true) == added);

        enabled.disable();
        CHECK(pushed(a, false)["devices"].is_array());
        CHECK(pushed(b, false)["op"] == "devices");
    }
    CoUninitialize();
}
#endif

TEST_CASE("input device settings: gain and channels come back per device on select, hot-plug and restart")
{
    TempDir dir;
    const fs::path file = dir.path / "audio-devices.json";
    const auto status = [](const std::string& uid, std::vector<int> channels, const std::string& state = "running") {
        azl::AudioInputStatus s;
        s.uid = uid;
        s.name = uid + " name";
        s.channels = std::move(channels);
        s.state = state;
        return s;
    };
    using Entry = azl::AudioDeviceSettings::Entry;
    {
        azl::AudioDeviceSettings devices(file, true, 0.0);
        // A device never set gets the default gain and keeps its channels.
        CHECK(devices.inputChanged(status("mixer", {1, 2})) == Entry{true, 0.0, {1, 2}});
        CHECK(!devices.inputChanged(status("mixer", {1, 2})));
        devices.gainChanged(false, 12.0);
        CHECK(!devices.inputChanged(status("mixer", {2})));
        CHECK(devices.find("mixer") == Entry{false, 12.0, {2}});

        // Another device has its own; the mixer's comes back when it is selected again or plugged back in.
        CHECK(devices.inputChanged(status("mic", {2}, "fallback")) == Entry{true, 0.0, {2}});
        devices.gainChanged(false, -6.0);
        CHECK(devices.inputChanged(status("mixer", {2})) == Entry{false, 12.0, {2}});
        CHECK(!devices.inputChanged(status("", {}, "none")));
        CHECK(devices.inputChanged(status("mic", {2})) == Entry{false, -6.0, {2}});
        CHECK(devices.inputChanged(status("mixer", {})) == Entry{false, 12.0, {2}});
    }
    // A restart reads them back from the engine machine's file.
    azl::AudioDeviceSettings restarted(file, true, 0.0);
    CHECK(restarted.find("mixer") == Entry{false, 12.0, {2}});
    CHECK(restarted.inputChanged(status("mixer", {})) == Entry{false, 12.0, {2}});
    CHECK(!restarted.find("other"));
    const nlohmann::json saved = nlohmann::json::parse(std::ifstream(file));
    CHECK(saved["devices"]["mixer"]["name"] == "mixer name");
    CHECK(saved["devices"]["mic"]["gainDb"] == -6.0);
}

TEST_CASE("input channel meter: silence, hiss and music read apart")
{
    const auto measure = [](auto signal) {
        azl::ChannelMeter meter;
        for (int i = 0; i < 12000; ++i)
            meter.add(signal(i));
        return meter.take();
    };
    std::mt19937 random(7);
    std::normal_distribution<float> hiss(0.0f, 0.001f);
    const azl::ChannelLevel silent = measure([](int) { return 0.0f; });
    const azl::ChannelLevel noise = measure([&](int) { return hiss(random); });
    const azl::ChannelLevel bass =
        measure([](int i) { return 0.5f * std::sin(2.0f * 3.14159265f * 80.0f * float(i) / 48000.0f); });
    CHECK(silent.levelDb == -120.0f);
    CHECK(noise.levelDb == doctest::Approx(-60.0).epsilon(0.05));
    CHECK(noise.noise > 0.9f);
    CHECK(bass.levelDb == doctest::Approx(-9.0).epsilon(0.05));
    CHECK(bass.noise < 0.01f);
}

TEST_CASE("audio settings with no fields answer the sender only; the state carries each input channel")
{
    AudioRig rig;
    Client a(rig.document);
    Client b(rig.document);
    const Message current = audio(a, {{"op", "settings"}});
    CHECK(current["op"] == "settings");
    CHECK(current["autoGain"] == true);
    const azl::ChannelLevel levels[] = {{-80.0f, 0.95f}, {-12.0f, 0.03f}};
    rig.analysis.setChannels(levels, 2);
    const Message state = audio(b, {{"op", "state"}});
    REQUIRE(state["channels"].size() == 2);
    CHECK(state["channels"][0]["levelDb"] == -80.0);
    CHECK(state["channels"][1]["noise"] == 0.03);
}
