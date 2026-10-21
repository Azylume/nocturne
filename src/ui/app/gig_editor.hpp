#pragma once

#include "ui/app/widgets.hpp"
#include "venues/gig.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace azl::ui {

// One night's Gig at the venue the UI shows: its date, window and a row per lineup slot (DJ, start, deck, five
// minutes earlier or later, remove). Save writes the Gig file on this machine and applies it to the venue in one undo
// step: its window and a deck switch Cue per slot. A shift saves at once, so its Cue follows.
class GigEditor : public juce::Component {
public:
    explicit GigEditor(Session& session, std::filesystem::path folder = defaultGigsFolder());

    void setFolder(std::filesystem::path folder) { folder_ = std::move(folder); }
    // The venue's Gig of `date`; without one its next Gig, or a new one for today.
    void load(std::optional<std::string> date = std::nullopt);
    const Gig& gig() const { return gig_; }
    // Why the last save did not go through; empty when it did.
    const juce::String& problem() const { return problem_; }
    bool save();
    void shift(size_t slot, int minutes);
    void addSlot();
    void removeSlot(size_t slot);
    std::function<void()> onClose;

    struct Row {
        juce::TextEditor dj, start;
        juce::TextButton deck, earlier{"-5"}, later{"+5"}, remove;
    };
    juce::TextEditor& dateField() { return date_; }
    // HH:MM-HH:MM
    juce::TextEditor& windowField() { return window_; }
    Row& row(size_t slot) { return *rows_[slot]; }
    size_t rowCount() const { return rows_.size(); }
    juce::TextButton& saveButton() { return save_; }
    juce::TextButton& addButton() { return add_; }
    juce::TextButton& closeButton() { return close_; }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void rebuild();
    // The Gig as the fields read; false with the problem set when they do not.
    bool readFields();
    juce::String deckName(uint64_t deck) const;

    Session& session_;
    std::filesystem::path folder_;
    Gig gig_;
    juce::String problem_;
    juce::TextEditor date_, window_;
    std::vector<std::unique_ptr<Row>> rows_;
    juce::TextButton add_{"+ Slot"}, save_{"Save"}, close_{"Close"};
};

} // namespace azl::ui
