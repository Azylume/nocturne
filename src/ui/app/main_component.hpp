#pragma once

#include "ui/app/alert_strip.hpp"
#include "ui/app/bottom_panes.hpp"
#include "ui/app/chain_pane.hpp"
#include "ui/app/clip_grid.hpp"
#include "ui/app/look.hpp"
#include "ui/app/machines_view.hpp"
#include "ui/app/map_view.hpp"
#include "ui/app/menu_bar.hpp"
#include "ui/app/monitors.hpp"
#include "ui/app/session.hpp"
#include "ui/app/tooltips.hpp"
#include "ui/app/transport_bar.hpp"

namespace azl::ui {

// The bar across the top of the bottom panes: dragging it resizes them.
class Splitter : public juce::Component, public juce::SettableTooltipClient {
public:
    std::function<void()> onStart;
    std::function<void(int dy)> onDrag;
    std::function<void()> onDone;
    Splitter()
    {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        setTooltip("Drag to resize the panes below the grid");
    }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent&) override;
};

// The one window: transport bar on top, the other venues' alerts under it while there are any, then Perform (monitors
// and audio on the left, the clip grid, three panes below it), Map or Machines. It fills the window with no page scroll. The bottom panes' height and whether the browser is
// hidden are remembered.
class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      public juce::ApplicationCommandTarget,
                      private juce::FocusChangeListener,
                      private Session::View {
public:
    // Without a settings file nothing is remembered.
    explicit MainComponent(Session& session, std::unique_ptr<juce::PropertiesFile> settings = {});
    ~MainComponent() override;

    void resized() override;
    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void setTab(TransportBar::Tab tab);
    // Map, or Perform.
    void setMapMode(bool map) { setTab(map ? TransportBar::Tab::Map : TransportBar::Tab::Perform); }
    void setBrowserHidden(bool hidden);
    // File > New: an empty composition (the screens stay), asking first over unsaved changes.
    void newComposition();
    // File > Open Recent: opens the show the way Open does, asking first over unsaved changes.
    void openRecent(const std::string& path);
    std::vector<RecentItem> recentItems() const;
    void clearRecent();

    ClipGrid& grid() { return grid_; }
    PreviewMonitor& preview() { return preview_; }
    ChainPane& chain() { return chain_; }
    MidPane& mid() { return mid_; }
    BrowserPane& browser() { return browser_; }
    AudioPanel& audio() { return audio_; }
    TransportBar& transport() { return transport_; }
    AlertStrip& alerts() { return alerts_; }
    MapView& map() { return map_; }
    MachinesView& machines() { return machines_; }
    CompositionMonitor& monitor() { return monitor_; }

    static std::unique_ptr<juce::PropertiesFile> defaultSettings();

    // Called when what a command's name, enabled or ticked state shows has changed.
    std::function<void()> onCommandsChanged;
    // The File, Edit and View menu commands and their shortcuts; the buttons' own handlers do the work.
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override;
    bool perform(const InvocationInfo& info) override;

private:
    bool commandEnabled(Command id) const;
    void refreshCommands();
    void globalFocusChanged(juce::Component*) override { refreshCommands(); }
    void sessionChanged(int changes) override;
    bool clipKey(const juce::KeyPress& key);

    Session& session_;
    std::unique_ptr<juce::PropertiesFile> settings_;
    Look look_;
    Tooltips tooltips_{this};
    TransportBar transport_;
    AlertStrip alerts_;
    CompositionMonitor monitor_;
    PreviewMonitor preview_;
    AudioPanel audio_;
    ClipGrid grid_;
    ChainPane chain_;
    MidPane mid_;
    BrowserPane browser_;
    BrowserStrip strip_;
    Splitter splitter_;
    MapView map_;
    MachinesView machines_;
    std::string commandState_;
    bool browserHidden_ = false;
    int bottomHeight_ = 0;
    int dragStartHeight_ = 0;
};

// The app's window around a MainComponent. When the system activates it, the keyboard focus goes to that
// MainComponent, so its shortcuts work before anything in the window was clicked.
class MainWindow : public juce::DocumentWindow, public juce::ApplicationCommandTarget {
public:
    MainWindow(Session& session, std::unique_ptr<juce::PropertiesFile> settings, bool visible);
    ~MainWindow() override;
    void focusGained(FocusChangeType cause) override;
    void closeButtonPressed() override;
    void resized() override;

    // The Close, Full Screen, Minimize and Zoom commands; the rest go on to the MainComponent.
    juce::ApplicationCommandTarget* getNextCommandTarget() override;
    void getAllCommands(juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override;
    bool perform(const InvocationInfo& info) override;
    juce::ApplicationCommandManager& commands() { return commands_; }
    MenuBar& menu() { return *menu_; }

private:
    virtual bool inFullScreen() const { return isFullScreen(); }

    juce::ApplicationCommandManager commands_;
    std::unique_ptr<MenuBar> menu_;
    bool fullScreen_ = false;
};

} // namespace azl::ui
