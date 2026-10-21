#pragma once

#include "ui/app/session.hpp"
#include "ui/app/widgets.hpp"

#include <deque>
#include <filesystem>
#include <map>
#include <set>

namespace azl::ui {

// The Machines tab: the health of the venue machine in view and its actions, in compact panels. Machine: render fps,
// GPU time, frame guard, CPU speed limit and temperature, version and the next Cue. Outputs: one row per output with
// its screen, display, presents and hitches per second and its state; a click on a row opens its raw numbers and its
// presents over the last minute. Audio: the input, its gain and level, the beat clock, and on the Director (an engine
// on another machine) Listen with its volume, to hear that input here about half a second late. While the DJ link is
// on, one row per player heard: on air, BPM and pitch, play state and beat in bar. The engine sends the
// status only while this tab is visible. Actions: Test card, Identify and Fix mirroring by click; Restart engine, Safe mode,
// Update and turning the Show lock off only by holding (HoldButton). While the Show lock is on, Restart, Safe mode and
// Update are greyed and the engine refuses them from every client; Update also inside a Gig window. Update sends the
// Nocturne.app beside this UI's own bundle (setUpdateBundle) and shows its progress and the installer's result.
// Media: the venue's media readiness (ready, or how many files are missing by deck), the download under way, the alert
// before a Gig, and the sync override (Auto, Allow, Block). Below the output rows, every output's return video side by
// side, each tile with its state; the engine streams it only while this tab is visible.
class MachinesView : public juce::Component, private Session::View {
public:
    explicit MachinesView(Session& session);
    ~MachinesView() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void visibilityChanged() override;

    // Each output row as drawn: "Screen 2 · Epson proj-b · 18.0/s · 0 hitches · low rate".
    juce::StringArray outputLines() const;
    // The Media panel's rows as drawn: readiness, sync state and any alert.
    juce::StringArray mediaLines() const;
    // One row per DJ link player as drawn: "Player 2 · on air · 128.00 BPM +0.00% · playing · 3/4".
    juce::StringArray djlinkLines() const;
    // The return video tile of each output, in row order; a null image while it has no picture.
    std::vector<juce::Image> tiles() const;
    // Opens or closes an output row's details, by its row.
    void toggleOutput(int row);
    bool expanded(int row) const;

    juce::TextButton& testCardButton() { return testCard_; }
    juce::TextButton& identifyButton() { return identify_; }
    juce::TextButton& unmirrorButton() { return unmirror_; }
    juce::TextButton& lockButton() { return lock_; }
    HoldButton& restartButton() { return restart_; }
    HoldButton& safeModeButton() { return safeMode_; }
    HoldButton& unlockButton() { return unlock_; }
    juce::TextButton& syncButton(const std::string& mode);
    HoldButton& updateButton() { return update_; }
    juce::TextButton& listenButton() { return listen_; }
    juce::Slider& volumeSlider() { return volume_; }
    void setUpdateBundle(std::filesystem::path app);
    // The Update's progress or result as the Actions panel shows it; empty when there is none.
    juce::String updateLine() const;

    static constexpr size_t kHistory = 120;

private:
    void sessionChanged(int changes) override;
    juce::Rectangle<int> machinePanel() const;
    juce::Rectangle<int> outputsPanel() const;
    juce::Rectangle<int> audioPanel() const;
    juce::Rectangle<int> actionsPanel() const;
    juce::Rectangle<int> mediaPanel() const;
    void updateActions();
    void startUpdate();
    std::pair<juce::String, juce::Colour> updateNote() const;
    void paintActions(juce::Graphics& g, const MachineStatus& status) const;
    void paintMachine(juce::Graphics& g, const MachineStatus& status) const;
    void paintOutputs(juce::Graphics& g, const MachineStatus& status) const;
    void paintAudio(juce::Graphics& g, const MachineStatus& status) const;
    void paintMedia(juce::Graphics& g, const MachineStatus::Media& media) const;
    int outputRowsHeight(const MachineStatus& status) const;
    juce::Rectangle<int> tilesArea() const;
    void paintTiles(juce::Graphics& g, const MachineStatus& status) const;
    void updateTiles();
    bool listenShown() const;

    Session& session_;
    std::optional<MachineStatus> status_;
    // By display: presents per second at each status, oldest first.
    std::map<std::string, std::deque<double>> history_;
    std::set<std::string> expanded_;
    // By display: the newest return video picture.
    std::map<std::string, juce::Image> tiles_;
    juce::TextButton testCard_{"Test card"};
    juce::TextButton identify_{"Identify"};
    juce::TextButton unmirror_{"Fix mirroring"};
    juce::TextButton lock_{"Show lock"};
    HoldButton restart_{"Restart engine"};
    HoldButton safeMode_{"Safe mode"};
    HoldButton unlock_{"Unlock show"};
    juce::TextButton syncAuto_{"Auto"};
    juce::TextButton syncAllow_{"Allow"};
    juce::TextButton syncBlock_{"Block"};
    HoldButton update_{"Update"};
    juce::TextButton listen_{"Listen"};
    juce::Slider volume_{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};
    std::filesystem::path updateBundle_;
    // The installer's last result when this UI started its Update, to tell the new one from it.
    std::optional<MachineStatus::Update> updateBefore_;
};

} // namespace azl::ui
