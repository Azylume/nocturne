#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"

#include <chrono>
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

bool enabled(azl::ui::MainWindow& window, azl::ui::Command id)
{
    juce::ApplicationCommandInfo info(id);
    window.getTargetForCommand(id)->getCommandInfo(id, info);
    return !(info.flags & juce::ApplicationCommandInfo::isDisabled);
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
