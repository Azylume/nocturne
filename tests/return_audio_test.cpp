#include "audio/opus_codec.hpp"
#include "audio/return_audio.hpp"
#include "net/websocket.hpp"
#include "test_client.hpp"

#include <atomic>
#include <cmath>
#include <functional>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kTone = 440.0;

std::vector<float> tone(double rate, double seconds, double amplitude = 0.5)
{
    std::vector<float> samples(size_t(rate * seconds));
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = float(amplitude * std::sin(2.0 * M_PI * kTone * double(i) / rate));
    return samples;
}

struct Heard {
    double rms = 0.0;
    double hz = 0.0;
};

// The level and the frequency, from the zero crossings, of 48 kHz samples after the first 0.1 s.
Heard hear(const std::vector<float>& samples)
{
    const size_t skip = size_t(0.1 * azl::kOpusRate);
    REQUIRE(samples.size() > skip + 4800);
    double power = 0.0;
    size_t crossings = 0;
    for (size_t i = skip; i < samples.size(); ++i) {
        power += double(samples[i]) * samples[i];
        crossings += (samples[i - 1] < 0.0f) != (samples[i] < 0.0f);
    }
    const double seconds = double(samples.size() - skip) / azl::kOpusRate;
    return {std::sqrt(power / double(samples.size() - skip)), double(crossings) / 2.0 / seconds};
}

// Every "listen" message the engine sends one in-process client.
class Listener {
public:
    explicit Listener(azl::DocumentServer& server)
        : server_(server), id_(server.connect([this](const Message& m) { push(m); }))
    {
        server_.receive(id_, msg::hello());
    }
    ~Listener() { server_.disconnect(id_); }

    void send(const Message& message) { server_.receive(id_, message); }

    std::vector<Message> audio()
    {
        std::lock_guard lock(mutex_);
        return audio_;
    }

    Message reply(const std::string& op)
    {
        const auto until = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < until) {
            {
                std::lock_guard lock(mutex_);
                for (const Message& m : replies_)
                    if (m.value("op", "") == op || m["type"] == "error")
                        return m;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL("no " << op << " reply");
        return {};
    }

private:
    void push(const Message& m)
    {
        std::lock_guard lock(mutex_);
        if (m["type"] == "listen" && m["op"] == "audio")
            audio_.push_back(m);
        else if (m["type"] == "listen" || m["type"] == "error")
            replies_.push_back(m);
    }

    azl::DocumentServer& server_;
    azl::ClientId id_;
    std::mutex mutex_;
    std::vector<Message> audio_;
    std::vector<Message> replies_;
};

// Feeds `samples` to the analysis in 10 ms chunks at about eight times real time, as the input would, pumping it on
// this thread; returns the longest push and pump.
double feed(azl::AudioAnalysis& analysis, const std::vector<float>& samples, double rate, double& time)
{
    const size_t chunk = size_t(rate / 100.0);
    double longest = 0.0;
    for (size_t at = 0; at + chunk <= samples.size(); at += chunk) {
        const auto start = Clock::now();
        analysis.push(samples.data() + at, chunk, time, rate);
        analysis.pump();
        longest = std::max(longest, std::chrono::duration<double>(Clock::now() - start).count());
        time += double(chunk) / rate;
        if (at % (chunk * 8) == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return longest;
}

std::vector<float> decodeAll(const std::vector<Message>& messages)
{
    azl::OpusStreamDecoder decoder;
    std::vector<float> pcm;
    for (const Message& m : messages) {
        const auto& payload = m[azl::kReturnPayload].get_binary();
        size_t offset = 0;
        for (const size_t size : m["sizes"]) {
            CHECK(decoder.decode(payload.data() + offset, size, pcm));
            offset += size;
        }
        CHECK(offset == payload.size());
    }
    return pcm;
}

bool waitFor(const std::function<bool()>& done, double seconds = 10.0)
{
    const auto until = Clock::now() + std::chrono::duration<double>(seconds);
    while (Clock::now() < until) {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

} // namespace

TEST_CASE("return audio: the system's Opus codec carries a tone through at 44.1 and 48 kHz, chunked as an input")
{
    for (const double rate : {44100.0, 48000.0}) {
        INFO(rate << " Hz in");
        azl::OpusStreamEncoder encoder(rate, azl::ReturnAudio::kBitrate);
        azl::OpusStreamDecoder decoder;
        const std::vector<float> in = tone(rate, 2.0);
        std::vector<float> out;
        size_t packets = 0, bytes = 0;
        for (size_t at = 0; at < in.size(); at += 441)
            for (const auto& packet : encoder.encode(in.data() + at, std::min<size_t>(441, in.size() - at))) {
                ++packets;
                bytes += packet.size();
                CHECK(decoder.decode(packet.data(), packet.size(), out));
            }
        CHECK(packets >= 95);
        CHECK(packets <= 100);
        CHECK(double(bytes) * 8.0 / 2.0 <= azl::ReturnAudio::kBitrate * 1.15);
        const Heard heard = hear(out);
        CHECK(heard.hz == doctest::Approx(kTone).epsilon(0.01));
        CHECK(heard.rms == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.1));
    }
}

TEST_CASE("return audio: a listening Director decodes the tone, within the bitrate bound; no listener, no audio bytes")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    azl::AudioAnalysis& analysis = harness.analysis();
    Listener idle(harness.document());
    double time = 1.0;

    feed(analysis, tone(48000.0, 2.0), 48000.0, time);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(harness.returnAudio().listeners() == 0);
    CHECK(harness.returnAudio().bytes() == 0);
    CHECK(idle.audio().empty());

    Listener director(harness.document());
    director.send({{"type", "listen"}, {"op", "start"}});
    const Message started = director.reply("start");
    REQUIRE(started["type"] == "listen");
    CHECK(started["codec"] == "opus");
    CHECK(started["bitrate"] == azl::ReturnAudio::kBitrate);
    CHECK(harness.returnAudio().listeners() == 1);

    const double seconds = 10.0;
    feed(analysis, tone(48000.0, seconds), 48000.0, time);
    REQUIRE(waitFor([&] { return director.audio().size() >= size_t(seconds * 50 / 3) - 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::vector<Message> heard = director.audio();
    CHECK(idle.audio().empty());
    CHECK(harness.returnAudio().dropped() == 0);

    uint64_t wire = 0;
    for (size_t i = 0; i < heard.size(); ++i) {
        CHECK(heard[i]["seq"] == heard[0]["seq"].get<uint64_t>() + i);
        CHECK(heard[i]["sizes"].size() == azl::ReturnAudio::kPacketsPerMessage);
        wire += azl::encode(heard[i], azl::Encoding::MessagePack).size();
    }
    const double kbps = double(wire) * 8.0 / 1000.0 / seconds;
    INFO(heard.size() << " messages, " << kbps << " kbit/s on the wire");
    CHECK(kbps <= azl::ReturnAudio::kMaxWireBitrate / 1000.0);
    CHECK(kbps >= 24.0);
    CHECK(harness.returnAudio().bytes() == wire);

    const Heard got = hear(decodeAll(heard));
    CHECK(got.hz == doctest::Approx(kTone).epsilon(0.01));
    CHECK(got.rms == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.1));

    director.send({{"type", "listen"}, {"op", "stop"}});
    CHECK(director.reply("stop")["listeners"] == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const size_t stoppedAt = director.audio().size();
    const uint64_t bytes = harness.returnAudio().bytes();
    feed(analysis, tone(48000.0, 1.0), 48000.0, time);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(director.audio().size() == stoppedAt);
    CHECK(harness.returnAudio().bytes() == bytes);
}

TEST_CASE("return audio: a Director that disconnects stops the stream")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    double time = 1.0;
    {
        Listener director(harness.document());
        director.send({{"type", "listen"}, {"op", "start"}});
        director.reply("start");
        feed(harness.analysis(), tone(48000.0, 0.5), 48000.0, time);
        REQUIRE(waitFor([&] { return !director.audio().empty(); }));
    }
    feed(harness.analysis(), tone(48000.0, 0.5), 48000.0, time);
    CHECK(waitFor([&] { return harness.returnAudio().listeners() == 0; }));
    const uint64_t bytes = harness.returnAudio().bytes();
    feed(harness.analysis(), tone(48000.0, 0.5), 48000.0, time);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(harness.returnAudio().bytes() == bytes);
}

TEST_CASE("return audio: a Director that stops reading never holds back the engine's audio analysis or its frames")
{
    azl::Composition c = azl::defaultComposition();
    c.screens.push_back({.id = 1, .name = "Screen", .display = "proj-1", .slices = {{.id = 1}}});
    azl::EngineHarness harness({.width = 480, .height = 270}, 60.0, {}, c);
    harness.step();
    harness.displays().attach({"proj-1", "proj-1", 480, 270, 60.0});
    azl::WebSocketServer server(harness.document(), 0);

    std::mutex mutex;
    std::condition_variable released;
    bool go = false;
    std::atomic<int> received{0};
    azl::WebSocketClient stalled("127.0.0.1", server.port(), azl::Encoding::MessagePack, [&](const Message&) {
        ++received;
        std::unique_lock lock(mutex);
        released.wait(lock, [&] { return go; });
    });
    struct Release {
        std::function<void()> run;
        ~Release() { run(); }
    } guard{[&] {
        {
            std::lock_guard lock(mutex);
            go = true;
        }
        released.notify_all();
    }};
    stalled.send(msg::hello());
    stalled.send({{"type", "listen"}, {"op", "start"}});
    REQUIRE(waitFor([&] { return harness.returnAudio().listeners() == 1; }));

    const std::vector<float> music = tone(48000.0, 6.0, 0.3);
    azl::AudioAnalysis reference([] { return 0.0; }, {.thread = false});
    reference.setSettings(harness.analysis().settings());
    double referenceTime = 1.0;
    for (size_t at = 0; at + 480 <= music.size(); at += 480) {
        reference.push(music.data() + at, 480, referenceTime, 48000.0);
        reference.pump();
        referenceTime += 0.01;
    }

    harness.startLive();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    harness.stats().maxGapMicros = 0;
    const uint64_t rendered = harness.stats().rendered;
    const auto started = Clock::now();
    double time = 1.0;
    const double longestPump = feed(harness.analysis(), music, 48000.0, time);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const uint64_t made = harness.stats().rendered - rendered;
    const double gapMs = harness.stats().maxGapMicros / 1000.0;
    harness.stopLive();

    INFO(made << " frames in " << seconds << " s, longest render gap " << gapMs << " ms, longest pump "
              << longestPump * 1000.0 << " ms, " << harness.returnAudio().bytes() << " bytes given to the transport");
    CHECK(received <= 1);
    CHECK(made / seconds > 55.0);
    CHECK(gapMs < 50.0);
    CHECK(longestPump < 0.02);
    CHECK(harness.returnAudio().dropped() == 0);
    CHECK(harness.returnAudio().bytes() > 20000);

    const azl::AudioFrame engine = harness.analysis().latest();
    const azl::AudioFrame alone = reference.latest();
    CHECK(engine.time == alone.time);
    CHECK(engine.values == alone.values);
    CHECK(engine.onsets == alone.onsets);
    CHECK(engine.spectrum == alone.spectrum);
}
