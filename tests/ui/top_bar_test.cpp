#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/look.hpp"
#include "ui/app/main_component.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>

namespace {

namespace fs = std::filesystem;

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

void click(juce::Button& button)
{
    button.triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
}

void shoot(juce::Component& c, const juce::String& name)
{
    const juce::Image shot = c.createComponentSnapshot(c.getLocalBounds(), true, 1.0f);
    const char* dir = std::getenv("NOCTURNE_UI_SHOTS");
    const fs::path path = fs::path(dir ? dir : NOCTURNE_UI_SHOTS) / (name + ".png").toStdString();
    fs::create_directories(path.parent_path());
    juce::File file(juce::String(path.string()));
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    REQUIRE(juce::PNGImageFormat().writeImageToStream(shot, out));
    MESSAGE("screenshot: " << file.getFullPathName());
}

} // namespace

namespace {

using azl::ui::Session;
using Group = Session::BarGroup;

std::vector<juce::Component*> groupParts(azl::ui::TransportBar& bar, Group group)
{
    if (group == Group::Sections)
        return {&bar.sectionButton(0), &bar.sectionButton(4), &bar.sectionMenuButton()};
    if (group == Group::Recording)
        return {&bar.recordButton()};
    return {&bar.tapButton(), &bar.resyncButton(), &bar.halveButton(), &bar.doubleButton()};
}

bool shown(azl::ui::TransportBar& bar, Group group)
{
    for (juce::Component* c : groupParts(bar, group))
        if (c->isVisible())
            return true;
    return false;
}

std::unique_ptr<juce::PropertiesFile> settingsAt(const juce::File& file)
{
    return std::make_unique<juce::PropertiesFile>(file, juce::PropertiesFile::Options{});
}

} // namespace

TEST_CASE("top bar: a group switched off in Settings hides, frees its space, persists and shows again")
{
    using namespace azl::ui;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    azl::WebSocketServer server(harness.document(), 0);
    const juce::File file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getNonexistentChildFile("nocturne-bar", ".settings", false);
    int fullLeft = 0;
    {
        auto settings = settingsAt(file);
        Session session("127.0.0.1", server.port());
        session.useSettings(settings.get());
        TransportBar bar(session);
        bar.setSize(1800, Look::metrics().transportHeight);
        REQUIRE(runUntil(harness, [&] { return session.client().connected(); }));
        for (Group g : {Group::Transport, Group::Sections, Group::Recording})
            CHECK(shown(bar, g));
        fullLeft = bar.statusLeft();

        bar.openSettings();
        SettingsModal& modal = *bar.settings();
        modal.showSection(SettingsModal::Section::TopBar);
        CHECK(modal.barToggle(Group::Sections).getToggleState());
        click(modal.barToggle(Group::Sections));
        CHECK_FALSE(session.barGroupShown(Group::Sections));
        CHECK_FALSE(shown(bar, Group::Sections));
        CHECK(shown(bar, Group::Transport));
        const int without = bar.statusLeft();
        CHECK(without < fullLeft - 200);

        click(modal.barToggle(Group::Recording));
        CHECK_FALSE(shown(bar, Group::Recording));
        CHECK(bar.statusLeft() < without);
        click(modal.barToggle(Group::Transport));
        CHECK_FALSE(shown(bar, Group::Transport));
        CHECK(bar.beatsArea().isEmpty());
        CHECK(bar.bpmArea().isEmpty());
        settings->saveIfNeeded();
    }
    {
        auto settings = settingsAt(file);
        Session session("127.0.0.1", server.port());
        session.useSettings(settings.get());
        TransportBar bar(session);
        bar.setSize(1800, Look::metrics().transportHeight);
        for (Group g : {Group::Transport, Group::Sections, Group::Recording})
            CHECK_FALSE(shown(bar, g));
        bar.openSettings();
        bar.settings()->showSection(SettingsModal::Section::TopBar);
        CHECK_FALSE(bar.settings()->barToggle(Group::Sections).getToggleState());
        click(bar.settings()->barToggle(Group::Sections));
        CHECK(shown(bar, Group::Sections));
        session.setBarGroupShown(Group::Transport, true);
        session.setBarGroupShown(Group::Recording, true);
        CHECK(bar.statusLeft() == fullLeft);
    }
    file.deleteFile();
}

TEST_CASE("top bar: it narrows by dropping the audio label, Previews, the beat and the nudge, and the Sections become "
          "one menu")
{
    using namespace azl::ui;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    TransportBar bar(session);
    REQUIRE(runUntil(harness, [&] { return session.client().connected(); }));
    bar.setSize(1800, Look::metrics().transportHeight);
    CHECK(bar.sectionButton(0).isVisible());
    CHECK_FALSE(bar.sectionMenuButton().isVisible());
    CHECK(bar.previewsButton().isVisible());
    CHECK(bar.nudgeButton(true).isVisible());
    CHECK_FALSE(bar.beatsArea().isEmpty());

    bar.setSize(1100, Look::metrics().transportHeight);
    CHECK_FALSE(bar.sectionButton(0).isVisible());
    CHECK(bar.sectionMenuButton().isVisible());
    CHECK(bar.beatsArea().isEmpty());
    CHECK(bar.tapButton().isVisible());
    CHECK(bar.recordButton().isVisible());

    const Menu menu = bar.sectionMenu();
    REQUIRE(menu.size() == 5);
    CHECK(menu[3].label == "Breakdown");

    bar.setSize(1440, Look::metrics().transportHeight);
    CHECK(bar.documentShown());

    for (int width = 1100; width <= 1800; width += 20) {
        bar.setSize(width, Look::metrics().transportHeight);
        CHECK(bar.settingsButton().getRight() <= width);
        CHECK(bar.settingsButton().getRight() <= bar.statusLeft());
    }
}

TEST_CASE("top bar: screenshots at three widths")
{
    using namespace azl::ui;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    TransportBar bar(session);
    REQUIRE(runUntil(harness, [&] { return session.client().connected(); }));
    const char* tag = std::getenv("NOCTURNE_SHOT_TAG");
    const juce::String prefix = juce::String("topbar-") + (tag ? tag : "after") + "-";
    const auto take = [&](int width, const juce::String& name) {
        bar.setSize(width, Look::metrics().transportHeight);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
        shoot(bar, prefix + name);
    };
    for (int width : {1800, 1440, 1000})
        take(width, juce::String(width));
    session.setBarGroupShown(Session::BarGroup::Sections, false);
    take(1440, "sections-off-1440");
    session.setBarGroupShown(Session::BarGroup::Transport, false);
    session.setBarGroupShown(Session::BarGroup::Recording, false);
    take(1440, "minimal-1440");
    session.setBarGroupShown(Session::BarGroup::Sections, true);
    session.setBarGroupShown(Session::BarGroup::Transport, true);
    session.setBarGroupShown(Session::BarGroup::Recording, true);
    MainComponent main(session);
    main.setSize(1440, 700);
    main.transport().openSettings();
    main.transport().settings()->showSection(SettingsModal::Section::TopBar);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    shoot(main, prefix + "settings-1440");
}
