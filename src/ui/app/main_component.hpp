#pragma once

#include "ui/app/alert_strip.hpp"
#include "ui/app/bottom_panes.hpp"
#include "ui/app/chain_pane.hpp"
#include "ui/app/clip_grid.hpp"
#include "ui/app/look.hpp"
#include "ui/app/machines_view.hpp"
#include "ui/app/map_view.hpp"
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
class MainComponent : public juce::Component, public juce::DragAndDropContainer, private Session::View {
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

private:
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
    bool browserHidden_ = false;
    int bottomHeight_ = 0;
    int dragStartHeight_ = 0;
};

} // namespace azl::ui
