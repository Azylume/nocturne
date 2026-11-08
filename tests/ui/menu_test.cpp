#include "net/websocket.hpp"
#include "test_client.hpp"
#include "test_files.hpp"
#include "ui/app/main_component.hpp"
#include "ui/app/recent_shows.hpp"

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

// The File menu's submenu of that name.
juce::PopupMenu subMenu(azl::ui::MainWindow& window, const juce::String& name)
{
    const juce::StringArray names = window.menu().getMenuBarNames();
    juce::PopupMenu popup = window.menu().getMenuForIndex(names.indexOf("File"), "File");
    for (juce::PopupMenu::MenuItemIterator it(popup); it.next();)
        if (it.getItem().text == name && it.getItem().subMenu)
            return *it.getItem().subMenu;
    return {};
}

std::vector<juce::PopupMenu::Item> itemsOf(const juce::PopupMenu& popup)
{
    std::vector<juce::PopupMenu::Item> items;
    for (juce::PopupMenu::MenuItemIterator it(popup); it.next();)
        if (!it.getItem().isSeparator)
            items.push_back(it.getItem());
    return items;
}

std::unique_ptr<juce::PropertiesFile> settingsAt(const std::filesystem::path& file)
{
    return std::make_unique<juce::PropertiesFile>(juce::File(file.string()), juce::PropertiesFile::Options());
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
    const auto collect = [&](const juce::PopupMenu& popup, const auto& again) -> void {
        for (juce::PopupMenu::MenuItemIterator it(popup); it.next();) {
            const auto& item = it.getItem();
            if (item.subMenu)
                again(*item.subMenu, again);
            else if (!item.isSeparator)
                CHECK(ids.insert(item.itemID).second);
        }
    };
    for (int i = 0; i < names.size(); ++i)
        collect(window.menu().getMenuForIndex(i, names[i]), collect);
    CHECK(ids.size() > 20);
}

TEST_CASE("menu: the recent shows keep ten, newest first, each once, and survive a restart")
{
    using azl::ui::RecentShows;
    TempDir dir;
    const auto file = dir.path / "UI.settings";
    {
        auto settings = settingsAt(file);
        RecentShows recent;
        recent.use(settings.get(), "Mini");
        CHECK(recent.paths().empty());
        for (int i = 0; i < 12; ++i)
            recent.add("/shows/" + std::to_string(i) + ".json");
        REQUIRE(recent.paths().size() == RecentShows::kMax);
        CHECK(recent.paths().front() == "/shows/11.json");
        CHECK(recent.paths().back() == "/shows/2.json");

        recent.add("/shows/5.json");
        const std::vector<std::string> paths = recent.paths();
        CHECK(paths.front() == "/shows/5.json");
        CHECK(paths.size() == RecentShows::kMax);
        CHECK(std::count(paths.begin(), paths.end(), "/shows/5.json") == 1);
        recent.add("");
        CHECK(recent.paths().size() == RecentShows::kMax);
    }
    {
        auto settings = settingsAt(file);
        RecentShows recent;
        recent.use(settings.get(), "mini");
        REQUIRE(recent.paths().size() == RecentShows::kMax);
        CHECK(recent.paths().front() == "/shows/5.json");
        CHECK(recent.paths()[1] == "/shows/11.json");

        recent.use(settings.get(), "other-host");
        CHECK(recent.paths().empty());
        recent.use(settings.get(), "mini");
        recent.clear();
        CHECK(recent.paths().empty());
    }
    auto settings = settingsAt(file);
    RecentShows recent;
    recent.use(settings.get(), "mini");
    CHECK(recent.paths().empty());
}

TEST_CASE("menu: Open Recent lists the shows saved and opened, greys one whose file is gone, and Clear Menu empties it")
{
    using namespace azl::ui;
    TempDir root;
    azl::EngineHarness harness({.width = 320, .height = 180}, 60.0, root.path);
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    MainWindow window(session, settingsAt(root.path / "UI.settings"), false);
    REQUIRE(runUntil(harness, [&] { return session.client().connected(); }));
    int changes = 0;
    struct Count : juce::MenuBarModel::Listener {
        int& n;
        explicit Count(int& n_) : n(n_) {}
        void menuBarItemsChanged(juce::MenuBarModel*) override { ++n; }
        void menuCommandInvoked(juce::MenuBarModel*, const juce::ApplicationCommandTarget::InvocationInfo&) override {}
    } count(changes);
    window.menu().addListener(&count);

    auto items = itemsOf(subMenu(window, "Open Recent"));
    REQUIRE(items.size() == 1);
    CHECK(items[0].text == "Clear Menu");
    CHECK_FALSE(items[0].isEnabled);

    const auto a = root.path / "a.json", b = root.path / "b.json";
    session.client().saveComposition(a.string());
    REQUIRE(runUntil(harness, [&] { return session.recentShows().paths().size() == 1; }));
    session.client().saveComposition(b.string());
    REQUIRE(runUntil(harness, [&] { return session.recentShows().paths().size() == 2; }));
    CHECK(changes > 0);

    items = itemsOf(subMenu(window, "Open Recent"));
    REQUIRE(items.size() == 3);
    CHECK(items[0].text == "b");
    CHECK(items[1].text == "a");
    CHECK(items[0].isEnabled);
    CHECK(items[1].isEnabled);
    CHECK(items[2].text == "Clear Menu");
    CHECK(items[2].isEnabled);

    std::filesystem::remove(a);
    items = itemsOf(subMenu(window, "Open Recent"));
    CHECK(items[0].isEnabled);
    CHECK_FALSE(items[1].isEnabled);

    window.menu().menuItemSelected(items[2].itemID, 0);
    CHECK(session.recentShows().paths().empty());
    items = itemsOf(subMenu(window, "Open Recent"));
    REQUIRE(items.size() == 1);
    CHECK_FALSE(items[0].isEnabled);
    window.menu().removeListener(&count);
}

TEST_CASE("menu: New, Import and an Open Recent item each do their one thing through the system menu's path")
{
    using namespace azl::ui;
    TempDir root;
    azl::EngineHarness harness({.width = 320, .height = 180}, 60.0, root.path);
    Client client{harness.document()};
    azl::WebSocketServer server(harness.document(), 0);
    Session session("127.0.0.1", server.port());
    MainWindow window(session, {}, false);
    REQUIRE(runUntil(harness, [&] { return session.client().connected(); }));

    std::vector<juce::String> asked, picked;
    std::vector<juce::File> toPick;
    bool agree = true;
    session.confirm = [&](const juce::String& question, const juce::String& action, std::function<void()> then) {
        asked.push_back(action + ": " + question);
        if (agree)
            then();
    };
    session.pickFiles = [&](const juce::String& title, const juce::File&, const juce::String&, bool,
                            std::function<void(std::vector<juce::File>)> done) {
        picked.push_back(title);
        if (!toPick.empty())
            done(toPick);
    };
    window.menu().openLink = [&](const juce::URL&) { FAIL("a File item opened a link"); };

    const auto pick = [&](int id) {
        const juce::StringArray names = window.menu().getMenuBarNames();
        const juce::PopupMenu file = window.menu().getMenuForIndex(names.indexOf("File"), "File");
        const auto find = [&](const juce::PopupMenu& popup, const auto& again) -> juce::PopupMenu::Item {
            for (juce::PopupMenu::MenuItemIterator it(popup); it.next();) {
                if (it.getItem().subMenu) {
                    if (auto found = again(*it.getItem().subMenu, again); found.itemID == id)
                        return found;
                } else if (it.getItem().itemID == id) {
                    return it.getItem();
                }
            }
            return {};
        };
        const juce::PopupMenu::Item item = find(file, find);
        REQUIRE(item.itemID == id);
        REQUIRE(item.isEnabled);
        if (item.commandManager)
            window.commands().invokeDirectly(Command(id), true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        window.menu().menuItemSelected(id, 0);
    };

    // New over a clean document asks nothing, and over an edit asks and then replaces; the file is forgotten.
    const auto show = root.path / "show.json";
    session.client().saveComposition(show.string());
    REQUIRE(runUntil(harness, [&] { return !session.client().savedPath().empty(); }));
    apply(client, azl::AddLayer{0, std::nullopt, azl::Layer{.name = "top"}});
    REQUIRE(runUntil(harness, [&] { return session.composition().layers.size() == 2; }));
    REQUIRE(session.client().unsaved());
    pick(CmdNew);
    CHECK(asked.size() == 1);
    CHECK(asked[0].startsWith("New: "));
    REQUIRE(runUntil(harness, [&] { return harness.document().document().layers.size() == 1; }));
    CHECK(session.client().savedPath().empty());
    CHECK_FALSE(session.client().unsaved());
    pick(CmdNew);
    CHECK(asked.size() == 1);

    // Declining the question leaves the document alone.
    apply(client, azl::AddLayer{0, std::nullopt, azl::Layer{.name = "top"}});
    REQUIRE(runUntil(harness, [&] { return session.composition().layers.size() == 2; }));
    agree = false;
    pick(CmdNew);
    CHECK(asked.size() == 2);
    harness.step(3);
    CHECK(harness.document().document().layers.size() == 2);

    // An Open Recent item opens that file through the same guard as Open.
    pick(2000);
    CHECK(asked.size() == 3);
    CHECK(asked[2].startsWith("Open: "));
    CHECK(session.composition().layers.size() == 2);
    agree = true;
    pick(2000);
    REQUIRE(runUntil(harness, [&] { return session.client().savedPath() == show.string(); }));
    CHECK(session.recentShows().paths().front() == show.string());
    CHECK(runUntil(harness, [&] { return session.composition().layers.size() == 1; }));

    // Import: each item opens its picker with nothing chosen and changes nothing.
    pick(CmdImportMedia);
    pick(CmdImportPresets);
    REQUIRE(picked.size() == 2);
    CHECK(picked[0].contains("media"));
    CHECK(picked[1].contains("presets"));
}
