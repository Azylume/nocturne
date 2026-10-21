#pragma once

#include "ui/app/session.hpp"

#include <functional>

namespace azl::ui {

// Open and Save as over the shows the engine lists on its machine: a searchable list (name, folder, date, size,
// missing media) where a click takes a show and a double-click opens or saves over it at once. Open keeps a path
// field for any other file; Save as names the file in a folder of the list. Opening over unsaved changes, and saving
// over a listed show, asks first. It covers its parent; closing hides it.
class ShowBrowser : public juce::Component, private Session::View, private juce::ListBoxModel {
public:
    ShowBrowser(Session& session, bool saving);
    ~ShowBrowser() override;

    juce::TextButton& actionButton() { return action_; }
    juce::TextButton& cancelButton() { return cancel_; }
    juce::TextEditor& searchField() { return search_; }
    // Open: the path on the engine machine. Save as: the file's name.
    juce::TextEditor& pathField() { return path_; }
    juce::ComboBox& folderBox() { return folder_; }
    juce::ListBox& list() { return list_; }
    // The shows the search leaves.
    const std::vector<ShowInfo>& rows() const { return rows_; }
    // What it asks before replacing, empty while it asks nothing.
    juce::String question() const { return question_.isVisible() ? question_.getText() : juce::String(); }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void sessionChanged(int changes) override;
    int getNumRows() override { return int(rows_.size()); }
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override { take(row); }
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    juce::String getTooltipForRow(int) override;
    void take(int row);
    void filter();
    void act();
    void ask(const juce::String& question, std::function<void()> then);
    void close();
    juce::Rectangle<int> panel() const;

    Session& session_;
    const bool saving_;
    ShowList shows_;
    std::vector<ShowInfo> rows_;
    std::vector<std::string> folders_;
    juce::Label title_, question_;
    juce::TextEditor search_, path_;
    juce::ComboBox folder_;
    juce::ListBox list_;
    juce::TextButton action_, cancel_{"Cancel"};
    std::function<void()> confirmed_;
};

} // namespace azl::ui
