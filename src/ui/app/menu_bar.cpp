#include "ui/app/menu_bar.hpp"

#include "ui/app/window_display.hpp"

namespace azl::ui {

namespace {

// JUCE also calls menuItemSelected for a command item with the command id, so these stay clear of the Command values.
constexpr int kShowWindow = 1001;
constexpr int kHelpDocs = 1002;
constexpr int kReportIssue = 1003;
const char* const kDocsUrl = "https://github.com/Azylume/nocturne/tree/main/docs";
const char* const kIssuesUrl = "https://github.com/Azylume/nocturne/issues/new";

constexpr int cmd = juce::ModifierKeys::commandModifier;
constexpr int shift = juce::ModifierKeys::shiftModifier;
constexpr int alt = juce::ModifierKeys::altModifier;
constexpr int ctrl = juce::ModifierKeys::ctrlModifier;

struct Description {
    const char* name;
    const char* category;
    juce::KeyPress key;
};

Description describe(Command id)
{
    switch (id) {
    case CmdOpen: return {"Open...", "File", {'o', cmd, 0}};
    case CmdSave: return {"Save", "File", {'s', cmd, 0}};
    case CmdSaveAs: return {"Save As...", "File", {'s', cmd | shift, 0}};
    case CmdClose: return {"Close", "File", {'w', cmd, 0}};
    case CmdUndo: return {"Undo", "Edit", {'z', cmd, 0}};
    case CmdRedo: return {"Redo", "Edit", {'z', cmd | shift, 0}};
    case CmdCut: return {"Cut", "Edit", {'x', cmd, 0}};
    case CmdCopy: return {"Copy", "Edit", {'c', cmd, 0}};
    case CmdPaste: return {"Paste", "Edit", {'v', cmd, 0}};
    case CmdSelectAll: return {"Select All", "Edit", {'a', cmd, 0}};
    case CmdSettings: return {"Settings...", "App", {',', cmd, 0}};
    case CmdViewPerform: return {"Perform", "View", {'1', cmd, 0}};
    case CmdViewMap: return {"Map", "View", {'2', cmd, 0}};
    case CmdViewMachines: return {"Machines", "View", {'3', cmd, 0}};
    case CmdToggleBrowser: return {"Browser", "View", {'b', cmd | alt, 0}};
    case CmdTogglePreviews: return {"Previews", "View", {'p', cmd | alt, 0}};
    case CmdFullScreen: return {"Enter Full Screen", "View", {'f', cmd | ctrl, 0}};
    case CmdMinimise: return {"Minimize", "Window", {'m', cmd, 0}};
    case CmdZoom: return {"Zoom", "Window", {}};
    }
    return {};
}

} // namespace

void describeCommand(Command id, juce::ApplicationCommandInfo& info, bool enabled, bool ticked, const juce::String& name)
{
    const Description d = describe(id);
    info.setInfo(name.isEmpty() ? juce::String(d.name) : name, {}, d.category, 0);
    if (d.key.isValid())
        info.addDefaultKeypress(d.key.getKeyCode(), d.key.getModifiers());
    if (id == CmdRedo)
        info.addDefaultKeypress('y', cmd);
    if (!enabled)
        info.setActive(false);
    if (ticked)
        info.setTicked(true);
}

MenuBar::MenuBar(juce::ApplicationCommandManager& commands, juce::TopLevelWindow& window)
    : commands_(commands), window_(window)
{
    setApplicationCommandManagerToWatch(&commands_);
#if JUCE_MAC
    useOwnFullScreenMenuItem();
    const juce::PopupMenu apple = appMenu();
    setMacMainMenu(this, &apple);
#endif
}

MenuBar::~MenuBar()
{
#if JUCE_MAC
    setMacMainMenu(nullptr);
#endif
}

juce::PopupMenu MenuBar::appMenu() const
{
    juce::PopupMenu menu;
    menu.addCommandItem(&commands_, CmdSettings);
    return menu;
}

juce::StringArray MenuBar::getMenuBarNames()
{
    return {"File", "Edit", "View", "Window", "Help"};
}

juce::PopupMenu MenuBar::getMenuForIndex(int, const juce::String& name)
{
    juce::PopupMenu menu;
    const auto add = [&](std::initializer_list<Command> ids) {
        for (Command id : ids)
            menu.addCommandItem(&commands_, id);
    };
    if (name == "File") {
        add({CmdOpen, CmdSave, CmdSaveAs});
        menu.addSeparator();
        add({CmdClose});
    } else if (name == "Edit") {
        add({CmdUndo, CmdRedo});
        menu.addSeparator();
        add({CmdCut, CmdCopy, CmdPaste, CmdSelectAll});
    } else if (name == "View") {
        add({CmdViewPerform, CmdViewMap, CmdViewMachines});
        menu.addSeparator();
        add({CmdToggleBrowser, CmdTogglePreviews});
        menu.addSeparator();
        add({CmdFullScreen});
    } else if (name == "Window") {
        add({CmdMinimise, CmdZoom});
        menu.addSeparator();
        menu.addItem(kShowWindow, window_.getName(), true, true);
    } else if (name == "Help") {
        menu.addItem(kHelpDocs, "Nocturne Documentation");
        menu.addItem(kReportIssue, "Report an Issue");
    }
    return menu;
}

void MenuBar::menuItemSelected(int itemId, int)
{
    if (itemId == kShowWindow)
        window_.toFront(true);
    else if (itemId == kHelpDocs)
        openLink(juce::URL(kDocsUrl));
    else if (itemId == kReportIssue)
        openLink(juce::URL(kIssuesUrl));
}

} // namespace azl::ui
