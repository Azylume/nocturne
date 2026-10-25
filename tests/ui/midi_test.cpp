#include <doctest.h>

#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"
#include "ui/app/midi.hpp"
#include "ui/app/midi_settings.hpp"

#include <functional>

namespace {

using azl::ui::MidiSource;
using azl::ui::MidiTarget;

bool runUntil(azl::EngineHarness& harness, const std::function<bool()>& done, double seconds = 20.0)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        harness.step();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(15);
        if (done())
            return true;
    }
    return false;
}

// Gives the UI and the engine time to settle, for checking that nothing happened.
void settle(azl::EngineHarness& harness, int ms = 300)
{
    runUntil(harness, [] { return false; }, ms / 1000.0);
}

juce::MidiMessage cc(int number, int value, int channel = 1)
{
    return juce::MidiMessage::controllerEvent(channel, number, value);
}

const azl::ui::MenuEntry* entry(const azl::ui::Menu& menu, const juce::String& label)
{
    for (const azl::ui::MenuEntry& e : menu)
        if (e.label == label)
            return &e;
    return nullptr;
}

void shoot(juce::Component& window, const std::string& name)
{
    const fs::path dir = NOCTURNE_UI_SHOTS;
    fs::create_directories(dir);
    juce::File file(juce::String((dir / name).string()));
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    REQUIRE(juce::PNGImageFormat().writeImageToStream(window.createComponentSnapshot(window.getLocalBounds(), true, 1.0f),
                                                      out));
    MESSAGE("screenshot: " << file.getFullPathName());
}

std::unique_ptr<juce::PropertiesFile> settingsAt(const fs::path& file)
{
    juce::PropertiesFile::Options options;
    return std::make_unique<juce::PropertiesFile>(juce::File(file.string()), options);
}

} // namespace

TEST_CASE("midi: a learned knob picks its parameter up before moving it, never jumps, and a turn is one undo step")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    Client client(harness.document());
    const uint64_t layer = harness.document().document().layers[0].id;
    apply(client, azl::SetLayerOpacity{layer, 0.8});
    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    REQUIRE(runUntil(harness, [&] { return session.composition().layers[0].opacity == 0.8; }));
    azl::ui::MidiControl& midi = session.midi();
    const auto opacity = [&] { return harness.document().document().layers[0].opacity; };

    // Learning takes the next knob, not a key.
    const MidiTarget target{.kind = MidiTarget::Kind::Param, .param = {layer, 0, "opacity"}};
    midi.learn(target);
    midi.handle(juce::MidiMessage::noteOn(1, 60, 1.0f));
    CHECK(midi.bindings().empty());
    midi.handle(cc(7, 0));
    REQUIRE(midi.bindings().size() == 1);
    CHECK(midi.bindings()[0].source == MidiSource{MidiSource::Kind::Cc, 1, 7});
    CHECK_FALSE(midi.learning());
    CHECK(midi.describe(target) == "Layer 1 / Opacity");

    // The knob starts far below the layer's 80 %: nothing moves until it reaches it.
    const size_t steps = session.client().history().undo;
    midi.handle(cc(7, 10));
    midi.handle(cc(7, 60));
    settle(harness);
    CHECK(opacity() == 0.8);
    // Past it, it takes over.
    midi.handle(cc(7, 110));
    REQUIRE(runUntil(harness, [&] { return std::abs(opacity() - 110 / 127.0) < 1e-9; }));
    midi.handle(cc(7, 64));
    REQUIRE(runUntil(harness, [&] { return std::abs(opacity() - 64 / 127.0) < 1e-9; }));
    // The turn ends as one undo step.
    REQUIRE(runUntil(harness, [&] { return session.client().history().undo == steps + 1; }));
    settle(harness, 600);
    CHECK(session.client().history().undo == steps + 1);

    // Moved by someone else, the layer stays where they put it until the knob comes back to it.
    apply(client, azl::SetLayerOpacity{layer, 0.2});
    REQUIRE(runUntil(harness, [&] { return session.composition().layers[0].opacity == 0.2; }));
    midi.handle(cc(7, 100));
    settle(harness);
    CHECK(opacity() == 0.2);
    midi.handle(cc(7, 20));
    REQUIRE(runUntil(harness, [&] { return std::abs(opacity() - 20 / 127.0) < 1e-9; }));

    // Another channel is another control.
    midi.handle(cc(7, 127, 2));
    settle(harness);
    CHECK(std::abs(opacity() - 20 / 127.0) < 1e-9);
}

TEST_CASE("midi: notes and pads fire columns and decks, macros follow a fader, and mappings survive a restart")
{
    TempDir dir;
    azl::EngineHarness harness({.width = 320, .height = 180});
    Client client(harness.document());
    const azl::Composition start = harness.document().document();
    const uint64_t column = start.decks[0].columns[2].id;
    const uint64_t layer = start.layers[0].id;
    apply(client, azl::SetSlot{layer, start.decks[0].columns[0].id,
                               azl::ClipRef{.name = "Solid", .effects = {{0, "solid", {}}}}});
    apply(client, azl::AddDeck{1, {100, "Night", {{101, "Late"}}}, {}});
    const uint64_t deck = harness.document().document().decks[1].id;
    azl::WebSocketServer server(harness.document(), 0);
    {
        azl::ui::Session session("127.0.0.1", server.port());
        REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
        azl::ui::MainComponent window(session, settingsAt(dir.path / "UI.settings"));
        window.setSize(1600, 950);
        REQUIRE(runUntil(harness, [&] { return session.composition().decks.size() == 2; }));
        azl::ui::MidiControl& midi = session.midi();

        // The column header's menu learns a key or a pad; a knob below half way is no press.
        const azl::ui::Menu columnMenu = window.grid().columns().columnMenu(2);
        REQUIRE(entry(columnMenu, "MIDI learn"));
        entry(columnMenu, "MIDI learn")->run();
        CHECK(session.status().startsWith("MIDI learn: move a control for "));
        midi.handle(cc(7, 40));
        CHECK(midi.bindings().empty());
        midi.handle(juce::MidiMessage::noteOn(10, 36, 0.8f));
        REQUIRE(midi.bindings().size() == 1);
        CHECK(entry(window.grid().columns().columnMenu(2), "Forget MIDI (Note 36 ch 10)"));
        midi.handle(juce::MidiMessage::noteOff(10, 36));
        settle(harness);
        CHECK(harness.document().document().column != column);
        midi.handle(juce::MidiMessage::noteOn(10, 36, 0.8f));
        REQUIRE(runUntil(harness, [&] { return harness.document().document().column == column; }));

        // A pad sending a CC fires once per press: when it passes half way up.
        entry(window.grid().decks().deckMenu(deck), "MIDI learn")->run();
        midi.handle(cc(20, 127, 10));
        REQUIRE(midi.bindings().size() == 2);
        midi.handle(cc(20, 0, 10));
        midi.handle(cc(20, 127, 10));
        REQUIRE(runUntil(harness, [&] { return harness.document().document().deck == deck; }));
        session.client().selectDeck(start.decks[0].id);
        REQUIRE(runUntil(harness, [&] { return session.composition().deck == start.decks[0].id; }));
        midi.handle(cc(20, 127, 10));
        settle(harness);
        CHECK(harness.document().document().deck == start.decks[0].id);

        // A macro knob: learning creates its macro; the fader then drives it once it picks it up at 0.
        const azl::ui::Menu knob = window.mid().macros().knobMenu(0);
        REQUIRE(entry(knob, "MIDI learn"));
        entry(knob, "MIDI learn")->run();
        REQUIRE(runUntil(harness, [&] { return session.composition().macros.size() == 1; }));
        midi.handle(juce::MidiMessage::pitchWheel(3, 0));
        REQUIRE(midi.bindings().size() == 3);
        midi.handle(juce::MidiMessage::pitchWheel(3, 16383));
        REQUIRE(runUntil(harness, [&] { return harness.document().document().macros[0].value == 1.0; }));

        // A cell's menu, and Tap's, offer it too; Esc cancels a learn.
        CHECK(entry(window.grid().stack().cellMenu(layer, 0), "MIDI learn"));
        CHECK(entry(window.transport().midiMenu(true), "MIDI learn"));
        entry(window.transport().midiMenu(true), "MIDI learn")->run();
        CHECK(midi.learning());
        window.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
        CHECK_FALSE(midi.learning());
        midi.handle(cc(1, 64));
        CHECK(midi.bindings().size() == 3);

        // Settings lists them; Remove takes one off.
        window.transport().openSettings();
        azl::ui::SettingsModal& modal = *window.transport().settings();
        modal.showSection(azl::ui::SettingsModal::Section::Midi);
        azl::ui::MidiSettings& panel = modal.midiSettings();
        REQUIRE(panel.rows() == 3);
        shoot(window, "midi-settings.png");
        CHECK(panel.rowText(0) == juce::String::fromUTF8("Note 36 ch 10 \xe2\x86\x92 ") + "Deck 1 / Column 3");
        CHECK(panel.inputsText() == "MIDI is off in this run");
        panel.removeButton(1).triggerClick();
        REQUIRE(runUntil(harness, [&] { return panel.rows() == 2; }));
        CHECK(midi.bindings().size() == 2);
    }
    // The UI starts again on the same settings: the mappings are there.
    auto settings = settingsAt(dir.path / "UI.settings");
    azl::ui::Session session("127.0.0.1", server.port());
    session.useSettings(settings.get());
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    REQUIRE(session.midi().bindings().size() == 2);
    CHECK(session.midi().bindings()[0].target == MidiTarget{.kind = MidiTarget::Kind::Column, .id = column});
    CHECK(session.midi().bindings()[1].source == MidiSource{MidiSource::Kind::PitchBend, 3, 0});
}

TEST_CASE("midi: a controller's messages arrive through CoreMIDI, and it is followed as it comes and goes")
{
    azl::EngineHarness harness({.width = 320, .height = 180});
    Client client(harness.document());
    const uint64_t layer = harness.document().document().layers[0].id;
    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    azl::ui::MidiControl& midi = session.midi();
    // Nothing listens before it starts.
    CHECK(midi.openInputs().isEmpty());

    const juce::String name = "Nocturne Test " + juce::String(juce::Random::getSystemRandom().nextInt(1 << 20));
    auto controller = juce::MidiOutput::createNewDevice(name);
    REQUIRE(controller);
    midi.start();
    REQUIRE(runUntil(harness, [&] { return midi.openInputs().contains(name); }, 10.0));

    midi.learn({.kind = MidiTarget::Kind::Param, .param = {layer, 0, "opacity"}});
    controller->sendMessageNow(cc(11, 127));
    REQUIRE(runUntil(harness, [&] { return midi.bindings().size() == 1; }, 10.0));
    // Opacity is 100 %: the knob at the top picks it up at once.
    controller->sendMessageNow(cc(11, 127));
    settle(harness);
    controller->sendMessageNow(cc(11, 30));
    REQUIRE(runUntil(harness, [&] { return std::abs(harness.document().document().layers[0].opacity - 30 / 127.0) < 1e-9; }));

    // Off closes it; unplugged, it is gone.
    midi.setInput("off");
    CHECK_FALSE(midi.openInputs().contains(name));
    midi.setInput("");
    CHECK(midi.openInputs().contains(name));
    controller.reset();
    REQUIRE(runUntil(harness, [&] { return !midi.openInputs().contains(name); }, 10.0));
}
