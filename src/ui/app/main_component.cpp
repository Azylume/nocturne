#include "ui/app/main_component.hpp"

#include "ui/app/midi.hpp"

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
    setWantsKeyboardFocus(true);
    setSize(1600, 950);
}

MainComponent::~MainComponent()
{
    session_.useSettings(nullptr);
    session_.removeView(this);
    setLookAndFeel(nullptr);
}

void MainComponent::sessionChanged(int changes)
{
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

} // namespace azl::ui
