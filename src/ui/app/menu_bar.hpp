#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace azl::ui {

enum Command : juce::CommandID {
    CmdOpen = 1,
    CmdSave,
    CmdSaveAs,
    CmdClose,
    CmdUndo,
    CmdRedo,
    CmdCut,
    CmdCopy,
    CmdPaste,
    CmdSelectAll,
    CmdSettings,
    CmdViewPerform,
    CmdViewMap,
    CmdViewMachines,
    CmdToggleBrowser,
    CmdTogglePreviews,
    CmdFullScreen,
    CmdMinimise,
    CmdZoom,
};

// Fills `info` for the commands of the app's menus and shortcuts: the name (`name` when given), the shortcut and, when
// `enabled` is false, the disabled flag.
void describeCommand(Command id, juce::ApplicationCommandInfo& info, bool enabled, bool ticked = false,
                     const juce::String& name = {});

// The menu bar of the window: File, Edit, View, Window and Help, every item a command of `commands`, so a menu pick and
// its shortcut run the same code as the buttons. On macOS it is the system menu bar; elsewhere it only owns the model.
class MenuBar : public juce::MenuBarModel {
public:
    MenuBar(juce::ApplicationCommandManager& commands, juce::TopLevelWindow& window);
    ~MenuBar() override;

    // What the Help items open; the default browser unless a test replaces it.
    std::function<void(const juce::URL&)> openLink = [](const juce::URL& url) { url.launchInDefaultBrowser(); };

    // The extra items of the application menu (Settings), after the system's About.
    juce::PopupMenu appMenu() const;
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int itemId, int index) override;

private:
    juce::ApplicationCommandManager& commands_;
    juce::TopLevelWindow& window_;
};

} // namespace azl::ui
