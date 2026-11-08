#pragma once

#include "ui/app/session.hpp"
#include "ui/app/settings_modal.hpp"
#include "ui/app/show_browser.hpp"
#include "ui/app/venue_menu.hpp"
#include "ui/app/widgets.hpp"

#include <functional>

namespace azl::ui {

// The top bar, in groups with a wider gap between them: the venue, the Perform, Map and Machines tabs and Live or
// Prepare (with Send and Discard in Prepare); the tempo with nudge (held), halve, double, tap and resync, the beat and
// quantisation; the Sections; recording, Previews and Settings; then the composition's name (with an unsaved dot) and the
// engine's status. Save, Open, Undo and Redo are in the menus. The tempo, Sections and recording groups can be switched
// off in Settings (Session::BarGroup) and take no space then; as the window narrows, the recording audio label,
// Previews, the beat, the Section buttons (into one menu button) and the nudge give way in that order.
class TransportBar : public juce::Component, private Session::View {
public:
    explicit TransportBar(Session& session);
    ~TransportBar() override;

    enum class Tab { Perform, Map, Machines };
    // Called when a tab button is clicked.
    std::function<void(Tab)> onTab;
    void setTab(Tab tab);
    juce::TextButton& tabButton(Tab tab) { return tab == Tab::Map ? map_ : tab == Tab::Machines ? machines_ : perform_; }
    // Called after the Previews button switched them, to remember it on this machine.
    std::function<void(bool)> onPreviews;
    // Saves to the file saved or opened last, else asks where.
    void save();
    // Over the window, the engine machine's shows to save to or open.
    void saveAs();
    void open();
    ShowBrowser* browser() { return browser_.get(); }
    // Over the window, the venues to switch the UI to.
    void openVenues();
    VenueButton& venueButton() { return venue_; }
    VenueMenu* venueMenu() { return venues_.get(); }
    // Over the window.
    void openSettings();
    SettingsModal* settings() { return settings_.get(); }
    juce::TextButton& settingsButton() { return settingsButton_; }

    void paint(juce::Graphics&) override;
    void resized() override;
    // Right-click on Tap or Resync: MIDI learn for it.
    void mouseDown(const juce::MouseEvent& e) override;
    Menu midiMenu(bool tap);
    juce::TextButton& tapButton() { return tap_; }
    juce::TextButton& resyncButton() { return resync_; }

    juce::TextButton& sectionButton(int section) { return *sections_[size_t(section)]; }
    // Stands for the five Section buttons when the bar is too narrow for them; a click opens their menu.
    juce::TextButton& sectionMenuButton() { return sectionMenu_; }
    Menu sectionMenu();
    // Where the beat and the BPM are drawn, empty while not shown, and where the engine's status begins.
    juce::Rectangle<int> beatsArea() const { return beats_; }
    juce::Rectangle<int> bpmArea() const { return bpm_; }
    int statusLeft() const { return status_.getX() - (doc_.isEmpty() ? 0 : doc_.getWidth()); }
    bool documentShown() const { return !doc_.isEmpty(); }
    juce::TextButton& nudgeButton(bool faster) { return faster ? faster_ : slower_; }
    juce::TextButton& halveButton() { return halve_; }
    juce::TextButton& doubleButton() { return double_; }
    juce::TextButton& previewsButton() { return previews_; }
    juce::TextButton& prepareButton() { return prepare_; }
    juce::TextButton& sendButton(bool atSwitch) { return atSwitch ? sendAtSwitch_ : sendNow_; }
    juce::TextButton& discardButton() { return discard_; }
    // The Section playing on the engine's timeline now, -1 before the first mark.
    int currentSection() const;
    juce::String documentName() const;
    // "guard: N layers half-res" while the engine's frame budget guard is active, else empty.
    juce::String guardText() const;
    // "Screen 2: display missing · Screen 3: display mirrored" for each screen whose display is not attached or mirrors
    // another, else empty.
    juce::String displayText() const;
    // What the status area says, and in which colour.
    std::pair<juce::String, juce::Colour> statusText() const;
    // Amber while recording with low disk space, red once a recording stopped on a full disk.
    juce::TextButton& recordButton() { return record_; }
    // Next to REC: the audio track it records, "audio off", or (amber) "no audio: <reason>"; while recording, when it
    // finishes by itself ("stops at 04:15") unless audio is missing.
    juce::Label& recordAudioLabel() { return recordAudio_; }

private:
    void sessionChanged(int changes) override;
    void showRecordSpace(const RecordState& record);
    void showRecordAudio(const RecordState& record);
    struct Slot;
    struct Plan;
    int layout(const Plan& plan, bool place);
    void paintBeats(juce::Graphics& g);
    void paintStatus(juce::Graphics& g);
    juce::Rectangle<int> statusArea() const { return status_; }
    void browse(bool saving);

    Session& session_;
    juce::TextButton perform_{"Perform"}, map_{"Map"}, machines_{"Machines"}, tap_{"TAP"}, resync_{"Resync"};
    juce::TextButton sectionMenu_{"Sections"}, slower_{juce::String::fromUTF8("\xe2\x88\x92")}, faster_{"+"};
    juce::TextButton halve_{"/2"}, double_{juce::String::fromUTF8("\xc3\x97" "2")};
    std::array<std::unique_ptr<juce::TextButton>, 5> sections_;
    juce::TextButton record_{"REC"}, previews_{"Previews"}, settingsButton_{"Settings"};
    juce::TextButton prepare_{"Prepare"}, sendNow_{"Send"}, sendAtSwitch_{"At switch"}, discard_{"Discard"};
    juce::Label recordAudio_;
    juce::ComboBox quantise_;
    juce::Rectangle<int> doc_, bpm_, beats_, status_;
    std::unique_ptr<ShowBrowser> browser_;
    std::unique_ptr<SettingsModal> settings_;
    VenueButton venue_;
    std::unique_ptr<VenueMenu> venues_;
};

} // namespace azl::ui
