#pragma once

#include "ui/app/gig_editor.hpp"
#include "ui/app/widgets.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace azl::ui {

// The Cues, each one sentence (when, do, on what) with its next firing, lit while it fires. Clicking a cue opens the
// builder under it, where each part of the sentence is a menu and every pick is an edit; "Add cue" opens it on a new
// cue that is added with Add. Hand-made Cues come first, then each Gig's lineup Cues under a header row; a click on
// the header or on one of them opens the Gig editor over the list. A cue at a time of day only shows a deck and fires
// a column, so its builder offers just those; no other cue offers a deck switch. Under the header of the night the
// engine's switch log belongs to, a collapsed row opens that night's deck switches, read-only, with their cause.
class CuesView : public juce::Component, public PointerTips, private Session::View {
public:
    // The parts of the sentence the builder picks, in order. Time replaces Bar and Repeat for a cue at a time of day;
    // Value, Ramp and Also show only for the kinds that have them (Also: the column a deck switch fires).
    enum class Part { When, Time, Bar, Repeat, Do, Value, Ramp, Target, Also };

    explicit CuesView(Session& session);
    ~CuesView() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String tipAt(juce::Point<int> point) override;

    // The beat the next firings count from and the highlight follows: the client's beat clock unless replaced.
    void setClock(std::function<double()> clock);

    juce::TextButton& addButton() { return add_; }
    // Add for a new cue, Done for one being edited.
    juce::TextButton& doneButton() { return done_; }
    juce::TextButton& cancelButton() { return cancel_; }
    // Opens the builder on the cue, or on a new one without an id.
    void open(std::optional<uint64_t> cue);
    void close();
    // The cue in the builder as it stands; none while it is closed.
    const std::optional<Cue>& draft() const { return draft_; }
    bool shows(Part part) const;
    juce::String pickLabel(Part part) const;
    Menu pickMenu(Part part);

    // The cue's row, its remove cross, its on/off toggle and its next firing, scrolled; a new cue's row follows the last.
    juce::Rectangle<int> rowArea(size_t index) const;
    juce::Rectangle<int> removeArea(size_t index) const;
    juce::Rectangle<int> toggleArea(size_t index) const;
    juce::Rectangle<int> nextArea(size_t index) const;
    juce::Rectangle<int> builderArea() const;
    juce::Rectangle<int> listArea() const;
    // The header row of the Gig of `date`; empty when no Cue of it is listed.
    juce::Rectangle<int> gigArea(const std::string& date) const;

    // The row that opens and closes the night's deck switches; empty while none are listed.
    juce::Rectangle<int> switchesArea() const;
    bool switchesOpen() const { return switchesOpen_; }
    // The switch rows shown, as they read.
    std::vector<juce::String> shownSwitches() const;
    // "03:00:12  Headliner  slot 2 Headliner  by cue"
    static juce::String switchText(const Message& line);

    juce::TextButton& gigButton() { return gigButton_; }
    GigEditor& gigEditor() { return gig_; }
    bool showsGig() const { return gig_.isVisible(); }
    // The Gig editor over the list, on the Gig of `date` or the venue's next one.
    void openGig(std::optional<std::string> date = std::nullopt);
    void closeGig();

    static juce::String describe(const Composition& composition, const Cue& cue);
    // "next: in 3 bars", "fired" while it holds, "waits for the Drop", "at 03:00", "off", or empty when it will not
    // fire again.
    static juce::String nextText(const Composition& composition, const Cue& cue, double beat);
    // Fired within the last bar.
    static bool firing(const Composition& composition, const Cue& cue, double beat);
    // Targets by deck: this deck's columns by position first, then a submenu per deck of its columns by name. No
    // level holds more than kMenuItems entries, with `spare` entries left free at the top level for the caller.
    static Menu columnMenu(const Composition& composition, const std::optional<CueColumn>& current,
                           const std::function<void(const CueColumn&)>& pick, size_t spare = 0);
    static constexpr size_t kMenuItems = 24;
    static constexpr const char* kThisDeck = "This deck (the one in view when it fires)";

private:
    struct Rows : juce::Component {
        explicit Rows(CuesView& owner);
        void paint(juce::Graphics& g) override { owner.paintRows(g); }
        CuesView& owner;
    };

    // A row of the list: a Cue by its index in the document (the new one's is past the end), a Gig's header, or under
    // it the deck switches row and, while open, a switch by its index in the log.
    struct Slot {
        std::optional<size_t> cue;
        std::string gig;
        bool switches = false;
        std::optional<size_t> line{};

        bool header() const { return !cue && !switches; }
    };

    void sessionChanged(int changes) override;
    void paintRows(juce::Graphics& g);
    std::vector<Slot> slots() const;
    juce::Rectangle<int> slotArea(size_t position) const;
    // Applies to the draft: a cue being edited is set at once, a new one waits for Add.
    void change(const std::function<void(Cue&)>& edit);
    Menu effectValueMenu(const Composition& composition, const CueEffect& current);
    void rebuild();
    // Lays the builder's pieces out in lines over `area` (when `place`); its height with the buttons below.
    int flowBuilder(juce::Rectangle<int> area, bool place) const;
    size_t rowCount() const;
    std::optional<size_t> draftRow() const;
    int contentHeight() const;

    Session& session_;
    Rows rows_{*this};
    juce::TextButton add_{"+ Add cue"};
    juce::TextButton done_, cancel_{"Cancel"}, gigButton_{"Gig"};
    GigEditor gig_;
    std::vector<std::unique_ptr<juce::Component>> pieces_;
    std::optional<Cue> draft_;
    std::function<double()> clock_;
    int scroll_ = 0;
    bool switchesOpen_ = false;
};

} // namespace azl::ui
