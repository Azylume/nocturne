#include "ui/app/bottom_panes.hpp"

#include "effects/effect_registry.hpp"
#include "ui/app/look.hpp"
#include "ui/app/widgets.hpp"

namespace azl::ui {

namespace {

constexpr int kRow = 20;
constexpr int kThumbWidth = 72;
constexpr int kTools = 28;
constexpr int kHint = 44;

} // namespace

TabbedPane::TabbedPane(juce::StringArray tabs) : tabs_(std::move(tabs)) {}

std::vector<juce::Rectangle<int>> TabbedPane::tabAreas() const
{
    std::vector<juce::Rectangle<int>> areas;
    int x = 0;
    for (int i = 0; i < tabs_.size(); ++i) {
        const int width = juce::GlyphArrangement::getStringWidthInt(Look::font(), tabName(i)) + 22;
        areas.emplace_back(x, 0, width, Look::metrics().paneTabsHeight);
        x += width + 1;
    }
    return areas;
}

juce::Rectangle<int> TabbedPane::body() const
{
    return getLocalBounds().withTrimmedTop(Look::metrics().paneTabsHeight);
}

void TabbedPane::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.panel);
    g.setColour(p.line);
    g.fillRect(getLocalBounds().withHeight(Look::metrics().paneTabsHeight));
    g.setFont(Look::font());
    const auto areas = tabAreas();
    for (int i = 0; i < int(areas.size()); ++i)
        fillWithText(g, areas[size_t(i)], i == tab_ ? p.panel : p.header, i == tab_ ? p.white : p.muted, tabName(i),
                     juce::Justification::centred, 0);
    const int right = areas.empty() ? 0 : areas.back().getRight() + 1;
    g.setColour(p.header);
    g.fillRect(juce::Rectangle<int>(right, 0, getWidth() - right, Look::metrics().paneTabsHeight));
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(body());
    paintBody(g, body());
}

void TabbedPane::mouseUp(const juce::MouseEvent& e)
{
    showTabAt(e.getPosition());
}

juce::String TabbedPane::tipAt(juce::Point<int> point)
{
    const auto areas = tabAreas();
    for (int i = 0; i < int(areas.size()); ++i)
        if (areas[size_t(i)].contains(point))
            return "Show " + tabName(i);
    return {};
}

void TabbedPane::showTabAt(juce::Point<int> point)
{
    const auto areas = tabAreas();
    for (int i = 0; i < int(areas.size()); ++i)
        if (areas[size_t(i)].contains(point) && i != tab_) {
            tab_ = i;
            tabChanged();
            repaint();
        }
}

MidPane::MidPane(Session& session)
    : TabbedPane({"Macros", "Autopilot", "Cues", "History", "Palette"}), session_(session), macros_(session),
      autopilot_(session), cues_(session), history_(session), palette_(session)
{
    addAndMakeVisible(macros_);
    addChildComponent(autopilot_);
    addChildComponent(cues_);
    addChildComponent(history_);
    addChildComponent(palette_);
    session_.addView(this);
}

MidPane::~MidPane()
{
    session_.removeView(this);
}

void MidPane::resized()
{
    for (juce::Component* c : std::initializer_list<juce::Component*>{&macros_, &autopilot_, &cues_, &history_, &palette_})
        c->setBounds(body().reduced(6));
}

void MidPane::tabChanged()
{
    macros_.setVisible(tab_ == 0);
    autopilot_.setVisible(tab_ == 1);
    cues_.setVisible(tab_ == 2);
    history_.setVisible(tab_ == 3);
    palette_.setVisible(tab_ == 4);
}

void MidPane::showTab(int tab)
{
    tab_ = tab;
    tabChanged();
    repaint();
}

void MidPane::showMacros()
{
    showTab(0);
}

juce::String MidPane::tabName(int index) const
{
    if (index == 2)
        return "Cues (" + juce::String(int(session_.composition().cues.size())) + ")";
    return TabbedPane::tabName(index);
}

void MidPane::sessionChanged(int changes)
{
    if (changes & Session::Document)
        repaint();
}

BrowserPane::BrowserPane(Session& session)
    : TabbedPane({"Files", "Effects", "Sources"}), session_(session), effects_(session)
{
    addChildComponent(effects_);
    effects_.query = [this] { return search_.getText(); };
    collapse_.setTooltip("Hide the browser");
    collapse_.onClick = [this] {
        if (onCollapse)
            onCollapse();
    };
    addAndMakeVisible(collapse_);
    addFiles_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Add media to the engine's library", juce::File(), "*.mp4;*.mov;*.m4v");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this](const juce::FileChooser& chooser) {
                                  std::vector<std::string> paths;
                                  for (const juce::File& file : chooser.getResults())
                                      paths.push_back(file.getFullPathName().toStdString());
                                  if (!paths.empty())
                                      session_.client().addMedia(paths, [](const LibraryItem&) {});
                              });
    };
    addAndMakeVisible(addFiles_);
    addFiles_.setTooltip("Copy movie files into the engine machine's Media Library");
    keepShow_.setClickingTogglesState(true);
    keepShow_.setTooltip("Keep every clip of this show on this machine; clips not kept show the engine's still");
    keepShow_.onClick = [this] { session_.keepShow(keepShow_.getToggleState()); };
    addAndMakeVisible(keepShow_);
    updateKeepShow();
    search_.setTooltip("Show only the clips, effects and presets whose names contain this");
    search_.setTextToShowWhenEmpty("Search", Look::palette().muted);
    search_.setFont(Look::font());
    search_.onTextChange = [this] {
        effects_.queryChanged();
        scroll_ = 0;
        repaint();
    };
    addAndMakeVisible(search_);
    session_.addView(this);
}

BrowserPane::~BrowserPane()
{
    session_.removeView(this);
}

void BrowserPane::resized()
{
    auto tools = body().removeFromTop(kTools).reduced(6, 4);
    addFiles_.setBounds(tools.removeFromRight(tab_ == 0 ? 110 : 0));
    tools.removeFromRight(tab_ == 0 ? 4 : 0);
    keepShow_.setBounds(tools.removeFromRight(tab_ == 0 ? 76 : 0));
    tools.removeFromRight(tab_ == 0 ? 4 : 0);
    search_.setBounds(tools);
    effects_.setBounds(body().withTrimmedTop(kTools).reduced(6, 0).withTrimmedBottom(kHint));
    collapse_.setBounds(getLocalBounds().removeFromTop(Look::metrics().paneTabsHeight).removeFromRight(24));
}

void BrowserPane::selectTab(int tab)
{
    tab_ = tab;
    tabChanged();
    repaint();
}

bool BrowserPane::matches(const juce::String& name) const
{
    const juce::String query = search_.getText().trim();
    return query.isEmpty() || name.containsIgnoreCase(query);
}

std::vector<std::pair<juce::Rectangle<int>, std::string>> BrowserPane::effectItems() const
{
    std::vector<std::pair<juce::Rectangle<int>, std::string>> result;
    if (tab_ == 1) {
        for (const auto& [bounds, row] : effects_.rows())
            if (row.kind == PresetRow::Kind::Effect)
                result.emplace_back(bounds + effects_.getPosition(), row.effectType);
        return result;
    }
    const auto list = listArea();
    int y = list.getY() - scroll_;
    std::vector<std::string> types = generatorTypes();
    types.push_back(kTextSource);
    for (const std::string& type : types) {
        if (!matches(type))
            continue;
        result.emplace_back(juce::Rectangle<int>(list.getX(), y, list.getWidth(), kRow), type);
        y += kRow + 1;
    }
    return result;
}

juce::Rectangle<int> BrowserPane::listArea() const
{
    return body().withTrimmedTop(tab_ == 0 ? kTools + 16 : kTools).reduced(6, 0);
}

int BrowserPane::listHeight() const
{
    int bottom = listArea().getY() - scroll_;
    if (tab_ == 2)
        for (const auto& [bounds, type] : effectItems())
            bottom = bounds.getBottom();
    else
        for (const FileItem& item : fileItems())
            bottom = std::max(bottom, item.bounds.getBottom());
    return bottom + scroll_ - listArea().getY();
}

void BrowserPane::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (tab_ == 1)
        return TabbedPane::mouseWheelMove(e, wheel);
    scroll_ = wheelScrolled(scroll_, wheel, listHeight(), listArea().getHeight());
    repaint();
}

std::optional<BrowserPane::FileItem> BrowserPane::fileAt(juce::Point<int> point) const
{
    if (tab_ == 0 && listArea().contains(point))
        for (const FileItem& item : fileItems())
            if (item.bounds.contains(point))
                return item;
    return std::nullopt;
}

std::optional<std::string> BrowserPane::sourceAt(juce::Point<int> point) const
{
    if (tab_ == 2 && listArea().contains(point))
        for (const auto& [bounds, type] : effectItems())
            if (bounds.contains(point))
                return type;
    return std::nullopt;
}

void BrowserPane::tabChanged()
{
    scroll_ = 0;
    addFiles_.setVisible(tab_ == 0);
    keepShow_.setVisible(tab_ == 0);
    effects_.setVisible(tab_ == 1);
    resized();
}

void BrowserPane::sessionChanged(int changes)
{
    if (changes & Session::Connection)
        addFiles_.setEnabled(session_.client().local());
    if (changes & (Session::Document | Session::Media | Session::Connection))
        updateKeepShow();
    if (changes & (Session::Library | Session::Stills | Session::Connection | Session::Media))
        repaint();
}

void BrowserPane::updateKeepShow()
{
    const std::string show = session_.currentShow();
    keepShow_.setEnabled(!show.empty());
    keepShow_.setToggleState(!show.empty() && session_.media().keep().show(show), juce::dontSendNotification);
}

juce::Rectangle<int> BrowserPane::keepDot(juce::Rectangle<int> tile)
{
    return tile.removeFromTop(12).removeFromRight(12).reduced(2);
}

void BrowserPane::paintKeepDot(juce::Graphics& g, juce::Rectangle<int> tile, const std::string& hash)
{
    const Palette& p = Look::palette();
    const auto dot = keepDot(tile).toFloat();
    g.setColour(juce::Colour(0xaa000000));
    g.fillEllipse(dot.expanded(1.5f));
    switch (session_.media().where(hash)) {
    case DirectorMedia::Where::Local:
        g.setColour(p.live);
        g.fillEllipse(dot);
        return;
    case DirectorMedia::Where::Queued:
    case DirectorMedia::Where::Fetching: {
        juce::Path half;
        half.addPieSegment(dot, 0.0f, juce::MathConstants<float>::pi, 0.0f);
        g.setColour(p.accent);
        g.fillPath(half);
        g.drawEllipse(dot, 1.0f);
        return;
    }
    case DirectorMedia::Where::Failed:
        g.setColour(p.fault);
        g.fillEllipse(dot);
        return;
    case DirectorMedia::Where::Remote:
        g.setColour(session_.media().keep().chosen(hash) ? p.accent : p.muted);
        g.drawEllipse(dot, 1.0f);
        return;
    }
}

juce::Rectangle<int> BrowserPane::uploadBadge(juce::Rectangle<int> tile)
{
    return tile.removeFromTop(12).removeFromLeft(12).reduced(2);
}

void BrowserPane::paintUploadBadge(juce::Graphics& g, juce::Rectangle<int> tile, const std::string& hash)
{
    const Palette& p = Look::palette();
    DirectorMedia& media = session_.media();
    const DirectorMedia::Upload upload = media.upload(hash);
    if (upload == DirectorMedia::Upload::Unknown)
        return;
    const auto badge = uploadBadge(tile).toFloat();
    g.setColour(juce::Colour(0xaa000000));
    g.fillEllipse(badge.expanded(1.5f));
    juce::Colour colour = p.muted;
    if (upload == DirectorMedia::Upload::Uploaded)
        colour = p.live;
    if (upload == DirectorMedia::Upload::Failed)
        colour = p.fault;
    if (upload == DirectorMedia::Upload::Uploading) {
        colour = p.accent;
        const UploadState state = *media.uploadState(hash);
        const float done = state.size ? float(double(state.bytes) / double(state.size)) : 0.0f;
        juce::Path ring;
        ring.addCentredArc(badge.getCentreX(), badge.getCentreY(), badge.getWidth() / 2, badge.getHeight() / 2, 0.0f,
                           0.0f, done * juce::MathConstants<float>::twoPi, true);
        g.setColour(colour);
        g.strokePath(ring, juce::PathStrokeType(1.5f));
    }
    const auto c = badge.getCentre();
    const float r = badge.getWidth() * 0.32f;
    juce::Path arrow;
    arrow.addTriangle(c.x - r, c.y, c.x + r, c.y, c.x, c.y - r);
    arrow.addRectangle(c.x - r * 0.35f, c.y, r * 0.7f, r);
    g.setColour(colour);
    g.fillPath(arrow);
}

static juce::String uploadTip(DirectorMedia& media, const std::string& hash)
{
    const std::optional<UploadState> state = media.uploadState(hash);
    switch (media.upload(hash)) {
    case DirectorMedia::Upload::LocalOnly:
        return "Only on this machine: not uploaded to the bucket yet";
    case DirectorMedia::Upload::Uploading:
        return "Uploading to the bucket: " +
               juce::String(state->size ? int(state->bytes * 100 / state->size) : 0) + "%";
    case DirectorMedia::Upload::Uploaded:
        return "In the bucket: every venue can fetch it";
    case DirectorMedia::Upload::Failed:
        return "Upload failed: " + utf8(state->error);
    case DirectorMedia::Upload::Unknown:
        break;
    }
    return {};
}

void BrowserPane::toggleKeep(const std::string& hash)
{
    const bool retry = session_.media().where(hash) == DirectorMedia::Where::Failed;
    session_.keepClip(hash, retry || !session_.media().keep().chosen(hash));
}

void BrowserPane::keepMenu(const std::string& hash)
{
    const std::optional<bool> own = session_.media().keep().clip(hash);
    juce::PopupMenu menu;
    menu.addItem("Keep on this machine", true, own == true, [this, hash] { session_.keepClip(hash, true); });
    menu.addItem("Don't keep on this machine", true, own == false, [this, hash] { session_.keepClip(hash, false); });
    menu.addItem("Follow the show", true, !own, [this, hash] { session_.keepClip(hash, std::nullopt); });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

static juce::String keepTip(DirectorMedia& media, const std::string& hash)
{
    switch (media.where(hash)) {
    case DirectorMedia::Where::Local:
        return "On this machine. Click to stop keeping it here; right-click to follow the show";
    case DirectorMedia::Where::Queued:
    case DirectorMedia::Where::Fetching:
        return "Coming to this machine from the bucket";
    case DirectorMedia::Where::Failed:
        return "Could not fetch it: " + utf8(media.fetchError(hash)) + ". Click to try again";
    case DirectorMedia::Where::Remote:
        break;
    }
    return "Not on this machine: the engine sends a still. Click to keep it here; right-click to follow the show";
}

// A folder's media are those in one directory on the engine machine.
static juce::String folderOf(const LibraryItem& item)
{
    const juce::String folder = juce::File(utf8(item.path)).getParentDirectory().getFileName();
    return item.path.empty() || folder.isEmpty() ? juce::String("Library") : folder;
}

std::vector<BrowserPane::FileItem> BrowserPane::fileItems() const
{
    std::vector<std::pair<juce::String, std::vector<LibraryItem>>> folders;
    for (const LibraryItem& item : session_.client().library()) {
        if (item.status != "ok" || !matches(utf8(item.name)))
            continue;
        const juce::String folder = folderOf(item);
        auto it = std::find_if(folders.begin(), folders.end(), [&](const auto& f) { return f.first == folder; });
        if (it == folders.end())
            it = folders.insert(folders.end(), {folder, {}});
        it->second.push_back(item);
    }
    std::vector<FileItem> result;
    const auto area = listArea();
    const int columns = std::max(1, area.getWidth() / (kThumbWidth + 2));
    const int width = (area.getWidth() - (columns - 1) * 2) / columns;
    const int height = width * 9 / 16;
    int y = area.getY() - scroll_;
    const bool searching = search_.getText().trim().isNotEmpty();
    for (const auto& [folder, items] : folders) {
        const int headerHeight = Look::metrics().chevronBox;
        result.push_back({{area.getX(), y, area.getWidth(), headerHeight}, folder, std::nullopt, items});
        y += headerHeight + 1;
        if (closed_.contains(folder) && !searching)
            continue;
        for (size_t i = 0; i < items.size(); ++i)
            result.push_back({{area.getX() + int(i % size_t(columns)) * (width + 2), y + int(i / size_t(columns)) * (height + 2),
                               width, height},
                              folder, items[i], {}});
        y += int((items.size() + size_t(columns) - 1) / size_t(columns)) * (height + 2) + 3;
    }
    return result;
}

void BrowserPane::paintBody(juce::Graphics& g, juce::Rectangle<int> area)
{
    const Palette& p = Look::palette();
    if (tab_ == 2) {
        g.reduceClipRegion(listArea());
        juce::Rectangle<int> last = area.withTrimmedTop(kTools).withHeight(0);
        for (const auto& [row, type] : effectItems()) {
            g.setFont(Look::font());
            fillWithText(g, row.withTrimmedLeft(row.getHeight() * 16 / 9 + 4), p.cell, p.text, generatorLabel(type));
            paintGenerator(g, row.withWidth(row.getHeight() * 16 / 9), Effect{0, type, {}});
            last = row;
        }
        g.setColour(p.muted);
        g.setFont(Look::font());
        g.drawFittedText("Drag onto a cell like a file; double-click adds it to the selected layer (Text: into the "
                         "selected clip's cell). Its settings are in the Clip tab.",
                         area.reduced(8, 0).withTop(last.getBottom() + 8).withHeight(48), juce::Justification::topLeft, 3);
        return;
    }
    if (tab_ == 1) {
        g.setColour(p.muted);
        g.setFont(Look::font());
        g.drawFittedText("Drag an effect or a preset onto a clip, a layer, the Composition header or the effect chain; "
                         "double-click adds to the chain shown.",
                         area.reduced(8, 0).withTop(effects_.getBottom() + 4).withHeight(kHint - 4),
                         juce::Justification::topLeft, 3);
        return;
    }
    g.setColour(p.muted);
    g.setFont(Look::smallFont());
    const auto library = session_.client().library();
    drawText(g, juce::String(int(library.size())) + juce::String::fromUTF8(" in the engine's library \xc2\xb7 drag a folder onto a layer"),
               area.withTrimmedTop(kTools).withHeight(16).reduced(8, 0), juce::Justification::centredLeft, true);
    const bool searching = search_.getText().trim().isNotEmpty();
    g.reduceClipRegion(listArea());
    for (const FileItem& item : fileItems()) {
        if (!item.bounds.intersects(listArea()))
            continue;
        if (!item.media) {
            const bool open = searching || !closed_.contains(item.folder);
            g.setFont(Look::font());
            drawHeader(g, item.bounds, open ? p.button : p.cell, open ? p.white : p.text, item.folder, open);
            g.setColour(p.muted);
            drawText(g, juce::String(int(item.folderItems.size())), item.bounds.reduced(6, 0), juce::Justification::centredRight);
            continue;
        }
        const auto& bounds = item.bounds;
        g.setColour(juce::Colours::black);
        g.fillRect(bounds);
        if (juce::Image still = session_.still(item.media->hash); still.isValid())
            g.drawImage(still, bounds.toFloat(), juce::RectanglePlacement::fillDestination);
        paintKeepDot(g, bounds, item.media->hash);
        paintUploadBadge(g, bounds, item.media->hash);
        auto caption = bounds.withTop(bounds.getBottom() - 13);
        g.setColour(juce::Colour(0xaa000000));
        g.fillRect(caption);
        g.setColour(p.text);
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        drawText(g, utf8(item.media->name), caption.reduced(2, 0), juce::Justification::centredLeft, true);
    }
}

void BrowserPane::mouseDown(const juce::MouseEvent& e)
{
    pressedEffect_ = sourceAt(e.getPosition());
    pressed_ = fileAt(e.getPosition());
    if (pressed_ && pressed_->media && e.mods.isPopupMenu()) {
        keepMenu(pressed_->media->hash);
        pressed_.reset();
    }
}

void BrowserPane::mouseUp(const juce::MouseEvent& e)
{
    TabbedPane::mouseUp(e);
    if (pressed_ && pressed_->media && e.getDistanceFromDragStart() < 5 &&
        keepDot(pressed_->bounds).expanded(2).contains(e.getMouseDownPosition()))
        toggleKeep(pressed_->media->hash);
    if (pressed_ && !pressed_->media && e.getDistanceFromDragStart() < 5) {
        if (!closed_.erase(pressed_->folder))
            closed_.insert(pressed_->folder);
        repaint();
    }
    pressed_.reset();
}

void BrowserPane::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (const auto item = fileAt(e.getPosition()); item && item->media)
        addToSelectedLayer({{item->media->hash, item->media->name}});
    const auto type = sourceAt(e.getPosition());
    const auto selected = session_.selectedClip();
    const auto at = selected ? session_.gridPosition(*selected) : std::nullopt;
    if (type == kTextSource && at)
        session_.placeClips(selected->layer, at->second, {generatorClip(*type)});
    else if (type)
        addToSelectedLayer({generatorClip(*type)});
}

juce::String BrowserPane::tipAt(juce::Point<int> point)
{
    if (const juce::String tab = TabbedPane::tipAt(point); tab.isNotEmpty())
        return tab;
    if (sourceAt(point))
        return "Drag onto a cell; double-click adds it to the selected layer";
    if (const auto item = fileAt(point); item && item->media && keepDot(item->bounds).expanded(2).contains(point))
        return keepTip(session_.media(), item->media->hash);
    if (const auto item = fileAt(point); item && item->media && uploadBadge(item->bounds).expanded(2).contains(point))
        if (const juce::String tip = uploadTip(session_.media(), item->media->hash); tip.isNotEmpty())
            return tip;
    if (const auto item = fileAt(point))
        return item->media ? "Drag onto a cell; double-click adds it to the selected layer"
                           : "Click to fold or unfold; drag onto a layer to place all of its clips";
    return {};
}

// Into the selected layer's first empty cells; without a selection, the top layer's.
void BrowserPane::addToSelectedLayer(const std::vector<ClipRef>& clips)
{
    std::optional<uint64_t> layer = session_.selectedLayer();
    if (!layer && !session_.composition().layers.empty())
        layer = session_.composition().layers.back().id;
    if (layer)
        session_.placeClips(*layer, -1, clips);
}

juce::String BrowserPane::describe(const std::vector<LibraryItem>& items)
{
    juce::String text = "media:";
    for (const LibraryItem& item : items)
        text += utf8(item.hash) + "\n" + utf8(item.name) + "\n";
    return text.trimEnd();
}

void BrowserPane::mouseDrag(const juce::MouseEvent& e)
{
    if (pressedEffect_ && e.getDistanceFromDragStart() >= 5) {
        for (const auto& [bounds, type] : effectItems())
            if (type == *pressedEffect_)
                startDrag(*this, "source:" + utf8(type), bounds, e);
        pressedEffect_.reset();
        return;
    }
    if (!pressed_ || e.getDistanceFromDragStart() < 5)
        return;
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this)) {
        const std::vector<LibraryItem> items = pressed_->media ? std::vector<LibraryItem>{*pressed_->media} : pressed_->folderItems;
        juce::Image still = items.empty() ? juce::Image() : session_.still(items.front().hash);
        container->startDragging(describe(items), this,
                                 juce::ScaledImage(still.isValid() ? still.rescaled(96, 54) : juce::Image()));
    }
    pressed_.reset();
}

} // namespace azl::ui

namespace azl::ui {

void BrowserStrip::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.header);
    juce::Graphics::ScopedSaveState state(g);
    g.addTransform(juce::AffineTransform::rotation(juce::MathConstants<float>::halfPi, 0.0f, 0.0f)
                       .translated(float(getWidth()), 0.0f));
    g.setColour(isMouseOver() ? p.white : p.muted);
    g.setFont(Look::font());
    drawText(g, "Browser", juce::Rectangle<int>(0, 0, getHeight(), getWidth()), juce::Justification::centred);
}

void BrowserStrip::mouseUp(const juce::MouseEvent&)
{
    if (onExpand)
        onExpand();
}

} // namespace azl::ui
