#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"

#include <cmath>
#include <functional>

namespace {

using azl::ui::Session;
using Tab = azl::ui::TransportBar::Tab;

bool runUntil(const std::function<bool()>& done, double seconds = 20.0)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        if (done())
            return true;
    }
    return false;
}

void writePng(juce::Component& component, const std::string& name)
{
    const char* dir = std::getenv("NOCTURNE_UI_SHOTS");
    const fs::path shots = dir ? dir : NOCTURNE_UI_SHOTS;
    fs::create_directories(shots);
    juce::File file(juce::String((shots / name).string()));
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    REQUIRE(juce::PNGImageFormat().writeImageToStream(
        component.createComponentSnapshot(component.getLocalBounds(), true, 1.0f), out));
    MESSAGE("screenshot: " << file.getFullPathName());
}

// A 440 Hz tone into the venue's analysis input, 20 ms at a time.
class TestTone {
public:
    explicit TestTone(azl::AudioAnalysis& analysis) : analysis_(analysis) {}

    void play()
    {
        float samples[960];
        for (float& s : samples)
            s = float(0.4 * std::sin(2.0 * M_PI * 440.0 * double(at_++) / 48000.0));
        analysis_.push(samples, 960, double(at_) / 48000.0, 48000.0);
        analysis_.pump();
    }

private:
    azl::AudioAnalysis& analysis_;
    uint64_t at_ = 0;
};

} // namespace

TEST_CASE("the Machines tab's Listen plays the venue's audio input on the Director, and only there")
{
    azl::EngineHarness harness({.width = 640, .height = 360});
    azl::WebSocketServer server(harness.document(), 0);
    TestTone tone(harness.analysis());

    SUBCASE("an engine on this machine shows no Listen")
    {
        Session session("127.0.0.1", server.port());
        azl::ui::MainComponent main(session);
        main.setSize(1600, 950);
        REQUIRE(runUntil([&] { return session.client().synced(); }));
        main.transport().tabButton(Tab::Machines).triggerClick();
        REQUIRE(runUntil([&] { return main.machines().restartButton().isVisible(); }));
        CHECK_FALSE(main.machines().listenButton().isVisible());
        CHECK_FALSE(main.machines().volumeSlider().isVisible());
    }

    SUBCASE("the Director hears it at the volume set, until Listen is off")
    {
        Session session("127.0.0.1", server.port(), false);
        session.returnAudio().useDevice(false);
        azl::ui::MainComponent main(session);
        main.setSize(1600, 950);
        REQUIRE(runUntil([&] { return session.client().synced(); }));
        main.transport().tabButton(Tab::Machines).triggerClick();
        azl::ui::MachinesView& machines = main.machines();
        REQUIRE(runUntil([&] { return machines.listenButton().isVisible(); }));
        CHECK(machines.volumeSlider().isVisible());
        CHECK(harness.returnAudio().listeners() == 0);

        machines.listenButton().triggerClick();
        REQUIRE(runUntil([&] { return session.listening(); }));
        CHECK(machines.listenButton().getToggleState());
        REQUIRE(runUntil([&] { return harness.returnAudio().listeners() == 1; }));
        REQUIRE(runUntil([&] {
            tone.play();
            return session.returnAudio().decodedSamples() >= 24000;
        }));
        CHECK(session.returnAudio().lostPackets() == 0);
        machines.volumeSlider().setValue(0.25);
        CHECK(runUntil([&] { return session.returnAudio().volume() == doctest::Approx(0.25); }));
        writePng(main, "machines-listen.png");

        machines.listenButton().triggerClick();
        REQUIRE(runUntil([&] { return !session.listening(); }));
        CHECK_FALSE(machines.listenButton().getToggleState());
        REQUIRE(runUntil([&] { return harness.returnAudio().listeners() == 0; }));
        const uint64_t bytes = harness.returnAudio().bytes();
        for (int i = 0; i < 50; ++i)
            tone.play();
        runUntil([] { return false; }, 0.2);
        CHECK(harness.returnAudio().bytes() == bytes);
    }
}
