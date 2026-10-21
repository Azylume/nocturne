#include "audio/audio_service.hpp"
#include "engine/render_thread.hpp"
#include "techno_loop.hpp"
#include "test_client.hpp"

#include <CoreAudio/CoreAudio.h>

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

bool realAudio()
{
    const char* value = std::getenv("NOCTURNE_REAL_AUDIO");
    return value && std::string(value) == "1";
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

TEST_CASE("an audio input never crashes on a missing device: it falls back to the default input or runs on nothing" *
          doctest::skip(!realAudio()))
{
    azl::AudioAnalysis analysis([] { return azl::engineTime(); });
    azl::AudioInput input(analysis);
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> events;
    input.onChange([&](const azl::AudioInputStatus&, const std::string& event) {
        std::lock_guard lock(mutex);
        events.push_back(event);
        changed.notify_all();
    });
    const auto selected = [&](const std::string& device) {
        std::unique_lock lock(mutex);
        events.clear();
        lock.unlock();
        input.select(device);
        lock.lock();
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(5), [&] {
            return std::any_of(events.begin(), events.end(), [](const std::string& e) { return e.starts_with("select"); });
        }));
        return input.status();
    };

    const azl::AudioInputStatus missing = selected("No Such Device 1234");
    CHECK(missing.requested == "No Such Device 1234");
    CHECK((missing.state == "fallback" || missing.state == "none"));
    CHECK(selected("off").state == "off");
    const azl::AudioInputStatus fallback = selected("");
    CHECK((fallback.state == "running" || fallback.state == "none"));
    for (const azl::AudioDeviceInfo& d : azl::AudioInput::devices())
        CHECK(d.channels > 0);
    input.onChange(nullptr);
}

TEST_CASE("NOCTURNE_NO_AUDIO_INPUT keeps every audio input off, whatever is selected" *
          doctest::skip(realAudio()))
{
    REQUIRE(azl::AudioInput::disabled());
    azl::AudioAnalysis analysis([] { return azl::engineTime(); });
    azl::AudioInput input(analysis);
    for (const std::string device : {"", "No Such Device 1234", "system"}) {
        const azl::AudioInputStatus status = selectedInput(input, device);
        CHECK(status.requested == "off");
        CHECK(status.state == "off");
        CHECK(status.uid.empty());
    }
}

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
