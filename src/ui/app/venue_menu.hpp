#pragma once

#include "ui/app/session.hpp"

#include <functional>

namespace azl::ui {

juce::Colour dotColour(VenueDot dot);

// The top bar's venue button: the dot and name of the venue the UI shows; a click opens the venue menu.
class VenueButton : public juce::Component, public juce::SettableTooltipClient, private Session::View {
public:
    explicit VenueButton(Session& session);
    ~VenueButton() override;

    std::function<void()> onClick;
    juce::String label() const;
    VenueDot dot() const;

    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }

private:
    void sessionChanged(int changes) override;

    Session& session_;
};

// Over the window, under the venue button: a row per venue with its dot, name, host and any build mismatch; a click
// connects the whole UI to that venue. A typed host connects too. A pairing token typed beside the host is kept for the
// host connected to next, picked or typed. It says plainly why Tailscale gave no list. A click beside it or Esc closes
// it.
class VenueMenu : public juce::Component, private Session::View, private juce::ListBoxModel {
public:
    explicit VenueMenu(Session& session);
    ~VenueMenu() override;

    juce::ListBox& list() { return list_; }
    juce::TextEditor& hostField() { return host_; }
    juce::TextEditor& tokenField() { return token_; }
    juce::TextButton& connectButton() { return connect_; }
    const std::vector<VenueEntry>& rows() const { return rows_; }
    // Why the list is short: no Tailscale key, a rejected key, no network, or no list in this run.
    juce::String note() const { return note_; }
    void pick(int row);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void sessionChanged(int changes) override;
    int getNumRows() override { return int(rows_.size()); }
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override { pick(row); }
    juce::String getTooltipForRow(int row) override;
    void connectTyped();
    void keepToken(const std::string& host);
    void close();
    juce::Rectangle<int> panel() const;

    Session& session_;
    std::vector<VenueEntry> rows_;
    juce::String note_;
    juce::ListBox list_;
    juce::TextEditor host_;
    juce::TextEditor token_;
    juce::TextButton connect_{"Connect"};
};

} // namespace azl::ui
