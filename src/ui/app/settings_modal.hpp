#pragma once

#include "ui/app/session.hpp"

#include <memory>
#include <vector>

namespace azl::ui {

// Settings over the window, one section per topic, each on the engine machine and kept there across restarts; each
// change goes to the engine at once, and a value an engine flag sets on each start says so. Recording: the folder
// recordings go to with its free space (or why it cannot record there), a picker over the engine machine's folders and
// volumes (the native folder dialog when the engine runs on this machine), and the recording settings. Audio input:
// the engine machine's input devices, the one in use with its channels, state and a level meter. Displays: which of the
// engine machine's displays each screen goes to, mirrored and missing ones marked, kept on that machine for every
// show with a screen of the same name. Performance: the render scale, the frame guard's
// profile and Protect transitions. Caches: the engine's cache bounds with what each holds now. Sections the engine
// does not serve are left out (an older engine: Recording only). It covers its parent; closing hides it.
class SettingsModal : public juce::Component,
                      private Session::View,
                      private juce::ListBoxModel,
                      private juce::Timer {
public:
    enum class Section { Recording, Audio, Displays, Performance, Caches };

    explicit SettingsModal(Session& session);
    ~SettingsModal() override;

    // Off: Choose always browses through the engine, as for an engine on another machine.
    void setNativeChooser(bool on) { native_ = on; }

    Section section() const { return section_; }
    void showSection(Section section);
    juce::TextButton& sectionButton(Section section);
    // The connected engine serves it.
    bool offers(Section section) const;

    juce::TextButton& chooseButton() { return choose_; }
    juce::TextButton& closeButton() { return close_; }
    juce::ComboBox& codecBox() { return codec_; }
    juce::ComboBox& sizeBox() { return size_; }
    juce::ToggleButton& audioToggle() { return audio_; }
    juce::ToggleButton& preciseToggle() { return precise_; }
    juce::TextEditor& reserveField() { return reserveMB_; }
    juce::TextEditor& reservePercentField() { return reservePercent_; }
    juce::TextEditor& maxSizeField() { return maxMB_; }
    juce::TextEditor& maxLengthField() { return maxSeconds_; }
    // In hours and minutes; the engine keeps seconds.
    juce::TextEditor& maxTotalField() { return maxTotal_; }
    juce::TextEditor& gigGraceField() { return gigGrace_; }
    // The folder and its free space, or why the engine cannot record there.
    juce::String folderText() const;

    // The engine-side picker: the folder it shows, its volumes then subfolders as rows; a click on a row enters it.
    bool picking() const { return picking_; }
    void browse(std::string path = {});
    void enter(int row);
    juce::ListBox& folderList() { return list_; }
    juce::TextButton& upButton() { return up_; }
    juce::TextButton& useButton() { return use_; }
    const FolderList& listed() const { return listed_; }

    // The default input, off, then the engine machine's input devices.
    juce::ComboBox& inputBox() { return input_; }
    // The input in use: device, channels and rate, and its state.
    juce::String inputText() const;
    // One per screen of the composition: none, then the engine machine's displays, a missing one last.
    int screenBoxes() const { return int(screens_.size()); }
    juce::ComboBox& screenBox(int index) { return *screens_[size_t(index)]; }
    juce::ComboBox& renderScaleBox() { return renderScale_; }
    juce::ComboBox& profileBox() { return profile_; }
    juce::ToggleButton& protectToggle() { return protect_; }
    juce::ToggleButton& bakeToggle() { return bake_; }
    juce::TextEditor& frameCacheField() { return frameCache_; }
    juce::TextEditor& preloadField() { return preload_; }
    juce::TextEditor& previewsField() { return previews_; }
    // What a cache holds now, next to its bound: "frameCacheMB", "preloadMB" or "clipPreviews".
    juce::String useText(const std::string& field) const;
    // "set by --flag on each start" for a field an engine flag set, else empty.
    juce::String overriddenText(const std::string& field) const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;

private:
    void sessionChanged(int changes) override;
    void timerCallback() override;
    juce::String freeText() const;
    int getNumRows() override { return int(listed_.volumes.size() + listed_.folders.size()); }
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override { enter(row); }
    const EngineFolder* row(int index) const;
    void choose();
    void pick(std::string folder);
    void showPicker(bool on);
    void refresh();
    void refreshAudio();
    void refreshDisplays();
    void refreshSettings();
    void updateVisibility();
    void send(const Message& fields);
    void sendNumbers();
    void sendCaches();
    void paintRecording(juce::Graphics& g);
    void paintAudio(juce::Graphics& g);
    void paintDisplays(juce::Graphics& g);
    void paintSettings(juce::Graphics& g);
    std::vector<juce::Component*> sectionComponents(Section section);
    juce::Rectangle<int> panel() const;
    juce::Rectangle<int> content() const;

    Session& session_;
    bool native_;
    bool picking_ = false;
    Section section_ = Section::Recording;
    RecordState record_;
    EngineSettings settings_;
    FolderList listed_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::Label title_;
    juce::TextButton recordingTab_{"Recording"}, audioTab_{"Audio input"}, displaysTab_{"Displays"},
        performanceTab_{"Performance"}, cachesTab_{"Caches"};
    juce::TextButton choose_{juce::String::fromUTF8("Choose\xe2\x80\xa6")}, close_{"Close"};
    juce::ComboBox codec_, size_;
    juce::ToggleButton audio_{"Audio track"}, precise_{"Precise (10/16-bit)"};
    juce::TextEditor reserveMB_, reservePercent_, maxMB_, maxSeconds_, maxTotal_, gigGrace_;
    juce::ListBox list_;
    juce::TextButton up_{"Up"}, use_{"Use this folder"}, cancel_{"Cancel"};
    juce::Rectangle<int> folderRow_, codecRow_, sizeRow_, reserveRow_, splitRow_, finishRow_, pathRow_;
    juce::ComboBox input_;
    std::vector<std::string> inputIds_;
    juce::String audioKey_;
    std::vector<std::unique_ptr<juce::ComboBox>> screens_;
    std::vector<uint64_t> screenIds_;
    std::vector<std::vector<std::string>> screenDisplays_;
    juce::ComboBox renderScale_;
    juce::ComboBox profile_;
    juce::ToggleButton protect_{"Protect transitions"};
    juce::ToggleButton bake_{"Bake static passes"};
    juce::TextEditor frameCache_, preload_, previews_;
};

} // namespace azl::ui
