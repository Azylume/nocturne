#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"

#include <chrono>
#include <cstdio>
#include <set>
#include <vector>
#include <functional>

namespace {

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

void invoke(azl::ui::MainWindow& window, azl::ui::Command id)
{
    window.commands().invokeDirectly(id, false);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
}

juce::ApplicationCommandInfo infoOf(azl::ui::MainWindow& window, azl::ui::Command id)
{
    juce::ApplicationCommandInfo info(id);
    window.commands().getTargetForCommand(id, info);
    return info;
}

bool enabled(azl::ui::MainWindow& window, azl::ui::Command id)
{
    return !(infoOf(window, id).flags & juce::ApplicationCommandInfo::isDisabled);
}

bool ticked(azl::ui::MainWindow& window, azl::ui::Command id)
{
    return infoOf(window, id).flags & juce::ApplicationCommandInfo::isTicked;
}

// The item of the menu as the menu bar builds it, what the system shows.
juce::PopupMenu::Item menuItem(azl::ui::MainWindow& window, const juce::String& menu, int id)
{
    const juce::StringArray names = window.menu().getMenuBarNames();
    juce::PopupMenu popup = window.menu().getMenuForIndex(names.indexOf(menu), menu);
    for (juce::PopupMenu::MenuItemIterator it(popup); it.next();)
        if (it.getItem().itemID == id)
            return it.getItem();
    return {};
}

// A window that says it is, or is not, in the system's full screen, as the green button would leave it.
class FullScreenWindow : public azl::ui::MainWindow {
public:
    explicit FullScreenWindow(azl::ui::Session& session) : MainWindow(session, {}, false) {}
    void setFullScreenNow(bool on)
    {
        full_ = on;
        resized();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    }

private:
    bool inFullScreen() const override { return full_; }
    bool full_ = false;
};

juce::String shortcut(const juce::PopupMenu::Item& item)
{
    juce::StringArray keys;
    if (item.commandManager)
        for (const juce::KeyPress& key : item.commandManager->getKeyMappings()->getKeyPressesAssignedToCommand(item.itemID))
            keys.add(key.getTextDescription());
    return keys.joinIntoString(", ");
}

void dumpMenu(const juce::String& menu, const juce::PopupMenu& popup)
{
    for (juce::PopupMenu::MenuItemIterator it(popup); it.next();) {
        const auto& item = it.getItem();
        if (!item.isSeparator)
            std::printf("menu | %s | %s | %s | %s | %s\n", menu.toRawUTF8(), item.text.toRawUTF8(),
                        shortcut(item).toRawUTF8(), item.isEnabled ? "enabled" : "disabled",
                        item.isTicked ? "ticked" : "");
    }
}

void dump(azl::ui::MainWindow& window)
{
    dumpMenu("Nocturne UI", window.menu().appMenu());
    const juce::StringArray names = window.menu().getMenuBarNames();
    for (int i = 0; i < names.size(); ++i)
        dumpMenu(names[i], window.menu().getMenuForIndex(i, names[i]));
}

} // namespace

TEST_CASE("menu: commands are disabled until the engine is there and its history has a step")
{
    using namespace azl::ui;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    Client client{harness.document()};
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    MainWindow window(session, {}, false);

    {
        Session offline("127.0.0.1", 1);
        MainWindow offlineWindow(offline, {}, false);
        CHECK_FALSE(enabled(offlineWindow, CmdOpen));
        CHECK_FALSE(enabled(offlineWindow, CmdSave));
        CHECK_FALSE(enabled(offlineWindow, CmdUndo));
    }
    REQUIRE(runUntil(harness, [&] { return session.client().connected(); }));
    CHECK(enabled(window, CmdOpen));
    CHECK(enabled(window, CmdSaveAs));
    CHECK_FALSE(enabled(window, CmdUndo));
    CHECK_FALSE(enabled(window, CmdRedo));

    apply(client, azl::SetLayerOpacity{1, 0.5});
    REQUIRE(runUntil(harness, [&] { return session.client().history().undo > 0; }));
    CHECK(enabled(window, CmdUndo));
    CHECK_FALSE(enabled(window, CmdRedo));

    session.client().undo();
    REQUIRE(runUntil(harness, [&] { return session.client().history().redo > 0; }));
    CHECK_FALSE(enabled(window, CmdUndo));
    CHECK(enabled(window, CmdRedo));
}

TEST_CASE("menu: clip commands and the browser toggle need the Perform view, and a view command switches to it")
{
    using namespace azl::ui;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    MainWindow window(session, {}, false);
    auto& main = *static_cast<MainComponent*>(window.getContentComponent());

    CHECK(enabled(window, CmdCopy));
    CHECK(enabled(window, CmdToggleBrowser));
    CHECK_FALSE(enabled(window, CmdCut));

    invoke(window, CmdViewMap);
    CHECK(main.map().isVisible());
    CHECK_FALSE(enabled(window, CmdCopy));
    CHECK_FALSE(enabled(window, CmdToggleBrowser));

    invoke(window, CmdViewPerform);
    CHECK(main.grid().isVisible());
    CHECK(enabled(window, CmdCopy));
}

TEST_CASE("menu: the View ticks follow the view, the browser and the previews however they were changed")
{
    using namespace azl::ui;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    MainWindow window(session, {}, false);
    auto& main = *static_cast<MainComponent*>(window.getContentComponent());
    const auto click = [](juce::Button& button) {
        button.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    };
    const auto ticks = [&](bool perform, bool map, bool machines) {
        return ticked(window, CmdViewPerform) == perform && ticked(window, CmdViewMap) == map &&
               ticked(window, CmdViewMachines) == machines &&
               menuItem(window, "View", CmdViewPerform).isTicked == perform &&
               menuItem(window, "View", CmdViewMap).isTicked == map &&
               menuItem(window, "View", CmdViewMachines).isTicked == machines;
    };

    CHECK(ticks(true, false, false));
    click(main.transport().tabButton(TransportBar::Tab::Map));
    CHECK(ticks(false, true, false));
    invoke(window, CmdViewMachines);
    CHECK(ticks(false, false, true));
    click(main.transport().tabButton(TransportBar::Tab::Perform));
    CHECK(ticks(true, false, false));

    CHECK(ticked(window, CmdToggleBrowser));
    invoke(window, CmdToggleBrowser);
    CHECK_FALSE(ticked(window, CmdToggleBrowser));
    main.setBrowserHidden(false);
    CHECK(menuItem(window, "View", CmdToggleBrowser).isTicked);

    const bool previews = session.previewsOn();
    click(main.transport().previewsButton());
    CHECK(menuItem(window, "View", CmdTogglePreviews).isTicked == !previews);
    invoke(window, CmdTogglePreviews);
    CHECK(menuItem(window, "View", CmdTogglePreviews).isTicked == previews);
}

TEST_CASE("menu: Full Screen reads Enter or Exit as the window is, and the menu bar is told when that changes")
{
    using namespace azl::ui;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    FullScreenWindow window(session);
    int changes = 0;
    struct Count : juce::MenuBarModel::Listener {
        int& n;
        explicit Count(int& n_) : n(n_) {}
        void menuBarItemsChanged(juce::MenuBarModel*) override { ++n; }
        void menuCommandInvoked(juce::MenuBarModel*, const juce::ApplicationCommandTarget::InvocationInfo&) override {}
    } count(changes);
    window.menu().addListener(&count);

    CHECK(menuItem(window, "View", CmdFullScreen).text == "Enter Full Screen");
    window.setFullScreenNow(true);
    CHECK(changes > 0);
    CHECK(menuItem(window, "View", CmdFullScreen).text == "Exit Full Screen");
    window.setFullScreenNow(false);
    CHECK(menuItem(window, "View", CmdFullScreen).text == "Enter Full Screen");
    window.menu().removeListener(&count);
}

TEST_CASE("menu: Undo and Redo name the step, and Settings is in the application menu and opens the modal")
{
    using namespace azl::ui;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    Client client{harness.document()};
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    MainWindow window(session, {}, false);
    auto& main = *static_cast<MainComponent*>(window.getContentComponent());
    REQUIRE(runUntil(harness, [&] { return session.client().connected(); }));

    CHECK(menuItem(window, "Edit", CmdUndo).text == "Undo");
    apply(client, azl::SetLayerOpacity{1, 0.5});
    REQUIRE(runUntil(harness, [&] { return session.client().history().undo > 0; }));
    CHECK(menuItem(window, "Edit", CmdUndo).text.startsWith("Undo "));
    CHECK(menuItem(window, "Edit", CmdUndo).isEnabled);

    juce::PopupMenu app = window.menu().appMenu();
    juce::PopupMenu::MenuItemIterator it(app);
    REQUIRE(it.next());
    CHECK(it.getItem().itemID == CmdSettings);
    CHECK(it.getItem().isEnabled);
    CHECK(window.commands().invokeDirectly(CmdSettings, false));
    CHECK(main.transport().settings() != nullptr);

    dump(window);
}

TEST_CASE("menu: a File item picked as the system menu does it runs its command and nothing else")
{
    using namespace azl::ui;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    MainWindow window(session, {}, false);
    auto& main = *static_cast<MainComponent*>(window.getContentComponent());
    REQUIRE(runUntil(harness, [&] { return session.client().connected(); }));
    std::vector<juce::String> links;
    window.menu().openLink = [&](const juce::URL& url) { links.push_back(url.toString(false)); };

    // The system runs the item's command, then tells the model the item's id.
    const auto pick = [&](const juce::String& menu, int id) {
        const juce::StringArray names = window.menu().getMenuBarNames();
        const int index = names.indexOf(menu);
        REQUIRE(menuItem(window, menu, id).isEnabled);
        window.commands().invokeDirectly(Command(id), true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        window.menu().menuItemSelected(id, index);
    };

    pick("File", CmdOpen);
    CHECK(main.transport().browser() != nullptr);
    pick("File", CmdSaveAs);
    CHECK(main.transport().browser() != nullptr);
    pick("File", CmdSave);
    CHECK(main.transport().browser() != nullptr);
    CHECK(menuItem(window, "File", CmdClose).isEnabled);
    CHECK(links.empty());

    pick("View", CmdViewMap);
    CHECK(main.map().isVisible());
    CHECK(links.empty());

    window.menu().menuItemSelected(1002, 4);
    CHECK(links.size() == 1);
}

TEST_CASE("menu: no two items of the menu bar share an id")
{
    using namespace azl::ui;
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    MainWindow window(session, {}, false);
    std::set<int> ids;
    const juce::StringArray names = window.menu().getMenuBarNames();
    for (int i = 0; i < names.size(); ++i) {
        juce::PopupMenu popup = window.menu().getMenuForIndex(i, names[i]);
        for (juce::PopupMenu::MenuItemIterator it(popup); it.next();)
            if (!it.getItem().isSeparator)
                CHECK(ids.insert(it.getItem().itemID).second);
    }
}
