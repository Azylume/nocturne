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

// A peer with no native window: the system's focus and key events go through JUCE's ComponentPeer as a real
// window's do, and nothing shows on the screen or takes the focus from another app.
class HeadlessPeer : public juce::ComponentPeer {
public:
    HeadlessPeer(juce::Component& component, int flags) : ComponentPeer(component, flags) {}
    void* getNativeHandle() const override { return nullptr; }
    void setVisible(bool) override {}
    void setTitle(const juce::String&) override {}
    void setBounds(const juce::Rectangle<int>& bounds, bool) override { bounds_ = bounds; }
    juce::Rectangle<int> getBounds() const override { return bounds_; }
    juce::Point<float> localToGlobal(juce::Point<float> p) override { return p + bounds_.getPosition().toFloat(); }
    juce::Point<float> globalToLocal(juce::Point<float> p) override { return p - bounds_.getPosition().toFloat(); }
    void setMinimised(bool) override {}
    bool isMinimised() const override { return false; }
    bool isShowing() const override { return true; }
    void setFullScreen(bool) override {}
    bool isFullScreen() const override { return false; }
    void setIcon(const juce::Image&) override {}
    bool contains(juce::Point<int> p, bool) const override { return bounds_.withZeroOrigin().contains(p); }
    OptionalBorderSize getFrameSizeIfPresent() const override { return {}; }
    juce::BorderSize<int> getFrameSize() const override { return {}; }
    bool setAlwaysOnTop(bool) override { return false; }
    void toFront(bool) override {}
    void toBehind(ComponentPeer*) override {}
    bool isFocused() const override { return true; }
    void grabFocus() override {}
    void repaint(const juce::Rectangle<int>&) override {}
    void performAnyPendingRepaintsNow() override {}
    void setAlpha(float) override {}
    juce::StringArray getAvailableRenderingEngines() override { return {}; }
    void textInputRequired(juce::Point<int>, juce::TextInputTarget&) override {}

private:
    juce::Rectangle<int> bounds_;
};

// The app's window on a HeadlessPeer.
class HeadlessWindow : public azl::ui::MainWindow {
public:
    explicit HeadlessWindow(azl::ui::Session& session) : MainWindow(session, {}, false)
    {
        removeFromDesktop();
        addToDesktop(getDesktopWindowStyleFlags());
        setVisible(true);
    }
    ~HeadlessWindow() override { removeFromDesktop(); }

    // As the system does when the window comes to the front, then for each key pressed with Cmd (Ctrl off Apple).
    void activate() { getPeer()->handleFocusGain(); }
    void command(char key, bool shift = false)
    {
        const int mods = juce::ModifierKeys::commandModifier | (shift ? juce::ModifierKeys::shiftModifier : 0);
        getPeer()->handleKeyPress(juce::KeyPress(key, mods, 0));
    }

private:
    juce::ComponentPeer* createNewPeer(int flags, void*) override { return new HeadlessPeer(*this, flags); }
};

} // namespace

TEST_CASE("keys: undo and redo keys work as soon as the window is in front, before anything in it was clicked")
{
    azl::EngineHarness harness{{.width = 320, .height = 180}};
    Client client{harness.document()};
    apply(client, azl::SetLayerOpacity{1, 0.5});
    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    HeadlessWindow window(session);
    REQUIRE(runUntil(harness, [&] { return session.client().connected(); }));
    const auto opacityIs = [&](double value) {
        return runUntil(harness, [&] { return harness.document().document().layers[0].opacity == value; });
    };

    window.activate();
    window.command('z');
    CHECK(opacityIs(1.0));
    window.command('z', true);
    CHECK(opacityIs(0.5));
    window.command('z');
    REQUIRE(opacityIs(1.0));
    window.command('y');
    CHECK(opacityIs(0.5));
}
