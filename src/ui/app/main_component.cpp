#include "ui/app/main_component.hpp"

#include "protocol/build_info.hpp"
#include "ui/app/midi.hpp"
#include "ui/app/window_display.hpp"

namespace azl::ui {

namespace {

constexpr int kMinBottom = 170;
constexpr int kStripWidth = 24;

} // namespace

void Splitter::paint(juce::Graphics& g)
{
    if (isMouseOverOrDragging())
        g.fillAll(Look::palette().live.withAlpha(0.2f));
}

void Splitter::mouseDown(const juce::MouseEvent&)
{
    if (onStart)
        onStart();
}

void Splitter::mouseDrag(const juce::MouseEvent& e)
{
    if (onDrag)
        onDrag(e.getDistanceFromDragStartY());
}

void Splitter::mouseUp(const juce::MouseEvent&)
{
    if (onDone)
        onDone();
}

std::unique_ptr<juce::PropertiesFile> MainComponent::defaultSettings()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "UI";
    options.folderName = "Nocturne";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    return std::make_unique<juce::PropertiesFile>(options);
}

MainComponent::MainComponent(Session& session, std::unique_ptr<juce::PropertiesFile> settings)
    : session_(session), settings_(std::move(settings)), transport_(session), alerts_(session), monitor_(session),
      preview_(session), audio_(session), grid_(session), chain_(session), mid_(session), browser_(session),
      map_(session), machines_(session)
{
    setLookAndFeel(&look_);
    for (juce::Component* child : std::initializer_list<juce::Component*>{&transport_, &monitor_, &preview_, &audio_, &grid_,
                                                                          &chain_, &mid_, &browser_, &splitter_})
        addAndMakeVisible(child);
    addChildComponent(alerts_);
    alerts_.onShownChanged = [this] { resized(); };
    addChildComponent(strip_);
    addChildComponent(map_);
    addChildComponent(machines_);
    transport_.onTab = [this](TransportBar::Tab tab) { setTab(tab); };
    audio_.onMacro = [this] { mid_.showMacros(); };
    browser_.onCollapse = [this] { setBrowserHidden(true); };
    strip_.onExpand = [this] { setBrowserHidden(false); };
    splitter_.onStart = [this] { dragStartHeight_ = chain_.getHeight(); };
    splitter_.onDrag = [this](int dy) {
        bottomHeight_ = juce::jlimit(kMinBottom, std::max(kMinBottom, getHeight() - 260), dragStartHeight_ - dy);
        resized();
    };
    splitter_.onDone = [this] {
        if (settings_)
            settings_->setValue("bottomHeight", bottomHeight_);
    };
    if (settings_) {
        bottomHeight_ = settings_->getIntValue("bottomHeight", 0);
        browserHidden_ = settings_->getBoolValue("browserHidden", false);
        session_.setPreviewsOn(settings_->getBoolValue("previews", true));
    }
    session_.useSettings(settings_.get());
    transport_.onPreviews = [this](bool on) {
        if (settings_)
            settings_->setValue("previews", on);
    };
    setBrowserHidden(browserHidden_);
    session_.addView(this);
    juce::Desktop::getInstance().addFocusChangeListener(this);
    setWantsKeyboardFocus(true);
    setSize(1600, 950);
}

MainComponent::~MainComponent()
{
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    onCommandsChanged = nullptr;
    session_.useSettings(nullptr);
    session_.removeView(this);
    setLookAndFeel(nullptr);
}

void MainComponent::sessionChanged(int changes)
{
    refreshCommands();
    if (changes & Session::Panels)
        repaint();
}

void MainComponent::setTab(TransportBar::Tab tab)
{
    transport_.setTab(tab);
    const bool perform = tab == TransportBar::Tab::Perform;
    for (juce::Component* child : std::initializer_list<juce::Component*>{&monitor_, &preview_, &audio_, &grid_, &chain_,
                                                                          &mid_, &splitter_})
        child->setVisible(perform);
    browser_.setVisible(perform && !browserHidden_);
    strip_.setVisible(perform && browserHidden_);
    map_.setVisible(tab == TransportBar::Tab::Map);
    machines_.setVisible(tab == TransportBar::Tab::Machines);
    refreshCommands();
}

void MainComponent::setBrowserHidden(bool hidden)
{
    browserHidden_ = hidden;
    if (settings_)
        settings_->setValue("browserHidden", hidden);
    setTab(map_.isVisible()        ? TransportBar::Tab::Map
           : machines_.isVisible() ? TransportBar::Tab::Machines
                                   : TransportBar::Tab::Perform);
    resized();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(Look::palette().line);
}

void MainComponent::paintOverChildren(juce::Graphics& g)
{
    if (!session_.learning())
        return;
    const Macro* macro = macroSlots(session_.composition())[size_t(session_.selectedMacro())];
    const juce::String name = macro ? utf8(macro->name) : "Macro " + juce::String(session_.selectedMacro() + 1);
    const juce::String text = "Assigning to " + name + juce::String::fromUTF8(": click parameter bars to add them \xc2\xb7 Esc to stop");
    const int width = juce::GlyphArrangement::getStringWidthInt(Look::font(true), text) + 28;
    const auto bar = juce::Rectangle<int>(width, 24).withCentre({getWidth() / 2, Look::metrics().transportHeight + 18});
    g.setFont(Look::font(true));
    fillWithText(g, bar, Look::palette().accent, juce::Colour(0xff1a1405), text, juce::Justification::centred, 0);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    transport_.setBounds(area.removeFromTop(Look::metrics().transportHeight));
    alerts_.setBounds(area.removeFromTop(alerts_.isVisible() ? AlertStrip::kHeight : 0));
    area.removeFromTop(1);
    map_.setBounds(area);
    machines_.setBounds(area);

    const int leftWidth = juce::jlimit(220, 300, area.getWidth() * 19 / 100);
    auto left = area.removeFromLeft(leftWidth);
    area.removeFromLeft(1);
    monitor_.setBounds(left.removeFromTop(monitor_.heightFor(left.getWidth())));
    left.removeFromTop(1);
    preview_.setBounds(left.removeFromTop(preview_.heightFor(left.getWidth())));
    left.removeFromTop(1);
    audio_.setBounds(left);

    const int fallback = juce::jlimit(220, 320, getHeight() * 33 / 100);
    const int bottomHeight = juce::jlimit(kMinBottom, std::max(kMinBottom, getHeight() - 260),
                                          bottomHeight_ > 0 ? bottomHeight_ : fallback);
    auto bottom = area.removeFromBottom(bottomHeight);
    area.removeFromBottom(1);
    grid_.setBounds(area);
    splitter_.setBounds(bottom.getX(), bottom.getY() - 4, bottom.getWidth(), 7);
    const int browserWidth = browserHidden_ ? kStripWidth : juce::jlimit(230, 300, getWidth() * 19 / 100);
    const int midWidth = juce::jlimit(330, 430, getWidth() * 28 / 100);
    const auto browserArea = bottom.removeFromRight(browserWidth);
    browser_.setBounds(browserArea);
    strip_.setBounds(browserArea);
    bottom.removeFromRight(1);
    mid_.setBounds(bottom.removeFromRight(midWidth));
    bottom.removeFromRight(1);
    chain_.setBounds(bottom);
    splitter_.toFront(false);
}

// The grid's shortcuts on the selected clips, shown in its menus.
bool MainComponent::clipKey(const juce::KeyPress& key)
{
    const int cmd = juce::ModifierKeys::commandModifier, shiftCmd = cmd | juce::ModifierKeys::shiftModifier;
    LayerStack& stack = grid_.stack();
    const auto clip = session_.selectedClip();
    const ChainOwner owner = clip ? ChainOwner{clip->layer, clip->column}
                                  : ChainOwner{session_.selectedLayer().value_or(kNoLayer)};
    if (key == juce::KeyPress('a', cmd, 0))
        session_.selectClips(session_.deckClips());
    else if (key == juce::KeyPress('c', cmd, 0))
        stack.copyClips();
    else if (key == juce::KeyPress('v', cmd, 0))
        stack.pasteClips();
    else if (key == juce::KeyPress('d', cmd, 0))
        stack.duplicateClips();
    else if (key == juce::KeyPress('r', cmd, 0))
        session_.repeatLastEdit();
    else if (key == juce::KeyPress('c', shiftCmd, 0))
        stack.copyEffects(owner);
    else if (key == juce::KeyPress('v', shiftCmd, 0))
        stack.pasteEffects(owner);
    else
        return false;
    return true;
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) {
        if (map_.isVisible() && map_.escape())
            return true;
        if (session_.midi().learning()) {
            session_.midi().cancelLearn();
            return true;
        }
        if (session_.learning())
            session_.setLearning(false);
        else
            session_.cancelQueued();
        session_.clearClipSelection();
        return true;
    }
    if (map_.isVisible() && map_.keyPressed(key))
        return true;
    if (map_.isVisible()) {
        const int step = key.getModifiers().isShiftDown() ? 10 : 1;
        const int code = key.getKeyCode();
        if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey ||
            code == juce::KeyPress::downKey)
            return map_.nudge(code == juce::KeyPress::leftKey ? -step : code == juce::KeyPress::rightKey ? step : 0,
                              code == juce::KeyPress::upKey ? -step : code == juce::KeyPress::downKey ? step : 0);
    }
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)) {
        session_.client().undo();
        return true;
    }
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0) ||
        key == juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0)) {
        session_.client().redo();
        return true;
    }
    if (key == juce::KeyPress(',', juce::ModifierKeys::commandModifier, 0)) {
        transport_.openSettings();
        return true;
    }
    if (key == juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0)) {
        transport_.save();
        return true;
    }
    if (key == juce::KeyPress('s', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)) {
        transport_.saveAs();
        return true;
    }
    if (key == juce::KeyPress('o', juce::ModifierKeys::commandModifier, 0)) {
        transport_.open();
        return true;
    }
    if (grid_.isVisible() && clipKey(key))
        return true;
    if (grid_.isVisible() && (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)) {
        grid_.stack().removeSelectedClip();
        return true;
    }
    return false;
}

namespace {

juce::TextEditor* focusedEditor()
{
    return dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent());
}

} // namespace

bool MainComponent::commandEnabled(Command id) const
{
    const bool connected = session_.client().connected();
    const History history = session_.client().history();
    const bool editing = focusedEditor() != nullptr;
    switch (id) {
    case CmdOpen:
    case CmdSave:
    case CmdSaveAs: return connected;
    case CmdUndo: return editing || (connected && history.undo > 0);
    case CmdRedo: return editing || (connected && history.redo > 0);
    case CmdCut: return editing;
    case CmdCopy:
    case CmdPaste:
    case CmdSelectAll: return editing || grid_.isVisible();
    case CmdToggleBrowser: return grid_.isVisible();
    default: return true;
    }
}

void MainComponent::refreshCommands()
{
    const History history = session_.client().history();
    const std::string state = std::to_string(session_.client().connected()) + std::to_string(focusedEditor() != nullptr) +
                              std::to_string(grid_.isVisible()) + std::to_string(map_.isVisible()) +
                              std::to_string(machines_.isVisible()) + std::to_string(browserHidden_) +
                              std::to_string(session_.previewsOn()) + "|" + std::to_string(history.undo) + history.undoOp +
                              "|" + std::to_string(history.redo) + history.redoOp;
    if (state == commandState_)
        return;
    commandState_ = state;
    if (onCommandsChanged)
        onCommandsChanged();
}

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& commands)
{
    commands.addArray({CmdOpen, CmdSave, CmdSaveAs, CmdUndo, CmdRedo, CmdCut, CmdCopy, CmdPaste, CmdSelectAll,
                       CmdSettings, CmdViewPerform, CmdViewMap, CmdViewMachines, CmdToggleBrowser, CmdTogglePreviews});
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto command = Command(id);
    const bool ticked = (command == CmdViewPerform && grid_.isVisible()) || (command == CmdViewMap && map_.isVisible()) ||
                        (command == CmdViewMachines && machines_.isVisible()) ||
                        (command == CmdToggleBrowser && grid_.isVisible() && !browserHidden_) ||
                        (command == CmdTogglePreviews && session_.previewsOn());
    const History history = session_.client().history();
    const juce::String name = command == CmdUndo && history.undo ? "Undo " + utf8(history.undoOp)
                              : command == CmdRedo && history.redo ? "Redo " + utf8(history.redoOp)
                                                                   : juce::String();
    describeCommand(command, info, commandEnabled(command), ticked, name);
}

bool MainComponent::perform(const InvocationInfo& info)
{
    juce::TextEditor* editor = focusedEditor();
    switch (Command(info.commandID)) {
    case CmdOpen: transport_.open(); break;
    case CmdSave: transport_.save(); break;
    case CmdSaveAs: transport_.saveAs(); break;
    case CmdUndo:
        if (editor)
            editor->undo();
        else
            session_.client().undo();
        break;
    case CmdRedo:
        if (editor)
            editor->redo();
        else
            session_.client().redo();
        break;
    case CmdCut:
        if (editor)
            editor->cut();
        break;
    case CmdCopy: editor ? editor->copy() : grid_.stack().copyClips(); break;
    case CmdPaste: editor ? editor->paste() : grid_.stack().pasteClips(); break;
    case CmdSelectAll:
        if (editor)
            editor->selectAll();
        else
            session_.selectClips(session_.deckClips());
        break;
    case CmdSettings: transport_.openSettings(); break;
    case CmdViewPerform: transport_.tabButton(TransportBar::Tab::Perform).triggerClick(); break;
    case CmdViewMap: transport_.tabButton(TransportBar::Tab::Map).triggerClick(); break;
    case CmdViewMachines: transport_.tabButton(TransportBar::Tab::Machines).triggerClick(); break;
    case CmdToggleBrowser: setBrowserHidden(!browserHidden_); break;
    case CmdTogglePreviews: transport_.previewsButton().triggerClick(); break;
    default: return false;
    }
    return true;
}

MainWindow::MainWindow(Session& session, std::unique_ptr<juce::PropertiesFile> settings, bool visible)
    : DocumentWindow("Nocturne " + juce::String(versionLabel()), Look::palette().background, DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    auto* main = new MainComponent(session, std::move(settings));
    setContentOwned(main, true);
    commands_.registerAllCommandsForTarget(this);
    commands_.registerAllCommandsForTarget(main);
    commands_.setFirstCommandTarget(this);
    main->onCommandsChanged = [this] { commands_.commandStatusChanged(); };
    menu_ = std::make_unique<MenuBar>(commands_, *this);
    setResizable(true, false);
    setResizeLimits(1100, 700, 10000, 10000);
    centreWithSize(getWidth(), getHeight());
    setVisible(visible);
}

MainWindow::~MainWindow()
{
    if (auto* main = dynamic_cast<MainComponent*>(getContentComponent()))
        main->onCommandsChanged = nullptr;
}

void MainWindow::resized()
{
    DocumentWindow::resized();
    if (fullScreen_ != inFullScreen()) {
        fullScreen_ = inFullScreen();
        commands_.commandStatusChanged();
    }
}

juce::ApplicationCommandTarget* MainWindow::getNextCommandTarget()
{
    return dynamic_cast<MainComponent*>(getContentComponent());
}

void MainWindow::getAllCommands(juce::Array<juce::CommandID>& commands)
{
    commands.addArray({CmdClose, CmdFullScreen, CmdMinimise, CmdZoom});
}

void MainWindow::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto command = Command(id);
    describeCommand(command, info, true, false,
                    command == CmdFullScreen && inFullScreen() ? "Exit Full Screen" : juce::String());
}

bool MainWindow::perform(const InvocationInfo& info)
{
    switch (Command(info.commandID)) {
    case CmdClose: closeButtonPressed(); break;
    case CmdFullScreen: setFullScreen(!inFullScreen()); break;
    case CmdMinimise: setMinimised(true); break;
    case CmdZoom:
        if (juce::ComponentPeer* peer = getPeer())
            zoomWindow(peer->getNativeHandle());
        break;
    default: return false;
    }
    return true;
}

void MainWindow::focusGained(FocusChangeType)
{
    getContentComponent()->grabKeyboardFocus();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

} // namespace azl::ui
