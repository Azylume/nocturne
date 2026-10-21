#pragma once

#include "ui/app/colour_picker.hpp"
#include "ui/app/cues_view.hpp"
#include "ui/app/macro_pane.hpp"
#include "ui/app/mid_tabs.hpp"
#include "ui/app/preset_tree.hpp"
#include "ui/app/session.hpp"

#include <vector>

namespace azl::ui {

// A pane with a row of tabs on top; the body is painted by the subclass.
class TabbedPane : public juce::Component, public PointerTips {
public:
    explicit TabbedPane(juce::StringArray tabs);
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;

protected:
    juce::Rectangle<int> body() const;
    virtual juce::String tabName(int index) const { return tabs_[index]; }
    virtual void paintBody(juce::Graphics& g, juce::Rectangle<int> area) = 0;
    virtual void tabChanged() {}
    // Switches to the tab under `point`, if any.
    void showTabAt(juce::Point<int> point);
    int tab_ = 0;

private:
    std::vector<juce::Rectangle<int>> tabAreas() const;
    juce::StringArray tabs_;
};

// Macros, Autopilot, Cues, History and Palette.
class MidPane : public TabbedPane, private Session::View {
public:
    explicit MidPane(Session& session);
    ~MidPane() override;
    void resized() override;
    MacrosView& macros() { return macros_; }
    AutopilotView& autopilot() { return autopilot_; }
    CuesView& cues() { return cues_; }
    HistoryView& history() { return history_; }
    PaletteView& palette() { return palette_; }
    void showMacros();
    void showTab(int tab);
    int tab() const { return tab_; }

private:
    void sessionChanged(int changes) override;
    juce::String tabName(int index) const override;
    void paintBody(juce::Graphics&, juce::Rectangle<int>) override {}
    void tabChanged() override;

    Session& session_;
    MacrosView macros_;
    AutopilotView autopilot_;
    CuesView cues_;
    HistoryView history_;
    PaletteView palette_;
};

// The engine's Media Library by folder as stills to drag onto cells (a folder drags all of its clips onto a layer;
// double-click adds a clip to the selected layer), the effects with their presets by group (PresetTree) and the
// generators with Text. The search field filters each tab by name. Each clip's corner dot tells whether its media is on
// this machine; clicking it keeps the clip here or not, right-click offers following the show, and Keep show keeps
// every clip of the open show. The opposite corner's arrow tells whether it is in the bucket: only here, going up with
// its progress, up, or failed.
class BrowserPane : public TabbedPane, private Session::View {
public:
    explicit BrowserPane(Session& session);
    ~BrowserPane() override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String tipAt(juce::Point<int> point) override;
    void selectTab(int tab);

    // Called when the hide button is clicked.
    std::function<void()> onCollapse;

    // A folder's header (no media) or one clip.
    struct FileItem {
        juce::Rectangle<int> bounds;
        juce::String folder;
        std::optional<LibraryItem> media;
        std::vector<LibraryItem> folderItems;
    };
    // Every folder and clip, scrolled; only those inside listArea show.
    std::vector<FileItem> fileItems() const;
    // The Effects tab's effect rows, or the Sources tab's generators and Text, scrolled.
    std::vector<std::pair<juce::Rectangle<int>, std::string>> effectItems() const;
    PresetTree& effects() { return effects_; }
    // Where the Files or Sources tab's list shows.
    juce::Rectangle<int> listArea() const;
    juce::TextEditor& search() { return search_; }
    juce::TextButton& keepShowButton() { return keepShow_; }
    // The corner dot of a clip's tile.
    static juce::Rectangle<int> keepDot(juce::Rectangle<int> tile);
    // The upload badge of a clip's tile.
    static juce::Rectangle<int> uploadBadge(juce::Rectangle<int> tile);
    // The drag description of these clips.
    static juce::String describe(const std::vector<LibraryItem>& items);

private:
    void sessionChanged(int changes) override;
    void paintBody(juce::Graphics& g, juce::Rectangle<int> area) override;
    void tabChanged() override;
    bool matches(const juce::String& name) const;
    int listHeight() const;
    std::optional<FileItem> fileAt(juce::Point<int> point) const;
    std::optional<std::string> sourceAt(juce::Point<int> point) const;
    void addToSelectedLayer(const std::vector<ClipRef>& clips);
    void toggleKeep(const std::string& hash);
    void keepMenu(const std::string& hash);
    void paintKeepDot(juce::Graphics& g, juce::Rectangle<int> tile, const std::string& hash);
    void paintUploadBadge(juce::Graphics& g, juce::Rectangle<int> tile, const std::string& hash);
    void updateKeepShow();

    Session& session_;
    juce::TextButton addFiles_{"Add from disk..."};
    juce::TextButton keepShow_{"Keep show"};
    juce::TextButton collapse_{juce::String::fromUTF8("\xc2\xbb")};
    juce::TextEditor search_;
    PresetTree effects_;
    int scroll_ = 0;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::optional<FileItem> pressed_;
    std::optional<std::string> pressedEffect_;
    std::set<juce::String> closed_;
};

// The browser hidden: a thin strip that brings it back when clicked.
class BrowserStrip : public juce::Component, public juce::SettableTooltipClient {
public:
    BrowserStrip() { setTooltip("Show the browser"); }
    std::function<void()> onExpand;
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
};

} // namespace azl::ui
