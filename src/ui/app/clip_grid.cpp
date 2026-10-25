#include "ui/app/clip_grid.hpp"

#include "ui/app/look.hpp"
#include "ui/app/widgets.hpp"

#include <cmath>
#include <utility>

namespace azl::ui {

namespace {

constexpr int kControlLine = 17;
constexpr int kControlHeight = 3 * kControlLine + 2;
constexpr int kButtonWidth = 18;
constexpr double kOpacitySendInterval = 1.0 / 30.0;

const Palette& palette()
{
    return Look::palette();
}

const Metrics& metrics()
{
    return Look::metrics();
}

double seconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

uint64_t maxId(const auto& items)
{
    uint64_t id = 0;
    for (const auto& item : items)
        id = std::max(id, item.id);
    return id;
}

// The layer's parent (0 for the composition) and its index there.
std::pair<uint64_t, size_t> placeOf(const Composition& composition, uint64_t layer)
{
    for (size_t i = 0; i < composition.layers.size(); ++i) {
        const Layer& outer = composition.layers[i];
        if (outer.id == layer)
            return {0, i};
        for (size_t j = 0; j < outer.layers.size(); ++j)
            if (outer.layers[j].id == layer)
                return {outer.id, j};
    }
    return {0, 0};
}

const std::vector<Layer>& siblings(const Composition& composition, uint64_t parent)
{
    for (const Layer& layer : composition.layers)
        if (layer.id == parent)
            return layer.layers;
    return composition.layers;
}

// "media:" then a hash and a name per clip, one per line.
std::vector<ClipRef> clipsOf(const juce::String& description)
{
    const juce::StringArray lines = juce::StringArray::fromLines(description.fromFirstOccurrenceOf("media:", false, false));
    std::vector<ClipRef> clips;
    for (int i = 0; i + 1 < lines.size(); i += 2)
        clips.push_back({lines[i].toStdString(), lines[i + 1].toStdString()});
    return clips;
}

juce::String autopilotText(const Layer& layer, uint64_t deck)
{
    const Autopilot autopilot = deckAutopilot(layer, deck);
    switch (autopilot.mode) {
    case AutopilotMode::Next: return "auto next " + juce::String(autopilot.bars);
    case AutopilotMode::Random: return "auto rnd " + juce::String(autopilot.bars);
    case AutopilotMode::Off: break;
    }
    return "auto";
}

// Video files dropped from the Finder, folders opened one level, in name order.
std::vector<std::string> mediaFiles(const juce::StringArray& dropped)
{
    std::vector<std::string> files;
    for (const juce::String& path : dropped) {
        juce::File file(path);
        if (!file.isDirectory()) {
            files.push_back(path.toStdString());
            continue;
        }
        juce::Array<juce::File> inside = file.findChildFiles(juce::File::findFiles, false, "*.mp4;*.mov;*.m4v");
        inside.sort();
        for (const juce::File& f : inside)
            files.push_back(f.getFullPathName().toStdString());
    }
    return files;
}

} // namespace

bool deckPlaying(const Composition& composition, const Deck& deck)
{
    bool playing = false;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (!layer.playing || !layer.clips.contains(layer.playing->column))
            return;
        for (const Column& column : deck.columns)
            playing = playing || column.id == layer.playing->column;
    });
    return playing;
}

DeckTabs::DeckTabs(Session& session) : session_(session)
{
    session_.addView(this);
}

DeckTabs::~DeckTabs()
{
    session_.removeView(this);
}

juce::String DeckTabs::badge(uint64_t deck) const
{
    const Composition& composition = session_.composition();
    const Autopilot& columns = composition.columnAutopilot;
    if (deck != composition.deck || columns.mode == AutopilotMode::Off)
        return {};
    return juce::String(columns.mode == AutopilotMode::Next ? "auto next " : "auto rnd ") + juce::String(columns.bars);
}

int DeckTabs::badgeWidth(uint64_t deck) const
{
    const juce::String text = badge(deck);
    return text.isEmpty() ? 0 : juce::GlyphArrangement::getStringWidthInt(Look::font(), text) + 10;
}

std::vector<std::pair<juce::Rectangle<int>, uint64_t>> DeckTabs::tabs() const
{
    std::vector<std::pair<juce::Rectangle<int>, uint64_t>> result;
    int x = 0;
    for (const Deck& deck : session_.composition().decks) {
        const int width =
            juce::GlyphArrangement::getStringWidthInt(Look::font(), utf8(deck.name)) + 40 + badgeWidth(deck.id);
        result.emplace_back(juce::Rectangle<int>(x, 0, width, getHeight()), deck.id);
        x += width + 1;
    }
    return result;
}

juce::Rectangle<int> DeckTabs::addButton() const
{
    const auto all = tabs();
    const int x = all.empty() ? 0 : all.back().first.getRight() + 1;
    return {x, 0, 34, getHeight()};
}

void DeckTabs::paint(juce::Graphics& g)
{
    const Palette& p = palette();
    const Composition& composition = session_.composition();
    g.fillAll(p.line);
    g.setFont(Look::font());
    for (const auto& [area, id] : tabs()) {
        const bool shown = id == composition.deck;
        const Deck* deck = nullptr;
        for (const Deck& d : composition.decks)
            if (d.id == id)
                deck = &d;
        const juce::String name = utf8(deck->name);
        g.setColour(shown ? p.live : p.button);
        g.fillRect(area);
        const int badged = badgeWidth(id);
        const juce::Rectangle<int> label = area.withTrimmedLeft(badged);
        if (badged > 0) {
            const auto mark = area.withWidth(badged).withTrimmedLeft(6).reduced(0, 5).toFloat();
            g.setColour(p.liveInk);
            g.fillRoundedRectangle(mark, 3.0f);
            g.setColour(p.live);
            drawText(g, badge(id), mark.toNearestInt(), juce::Justification::centred);
        }
        fillWithText(g, label, shown ? p.live : p.button, shown ? p.liveInk : p.text, name, juce::Justification::centred);
        if (deckPlaying(composition, *deck)) {
            const int right = label.getCentreX() + juce::GlyphArrangement::getStringWidthInt(Look::font(), name) / 2;
            g.setColour(shown ? p.liveInk : p.live);
            g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre({float(right + 8), float(area.getCentreY())}));
        }
    }
    if (const auto all = tabs(); dropGap_ && !all.empty()) {
        const int x = *dropGap_ < all.size() ? all[*dropGap_].first.getX() - 1 : all.back().first.getRight();
        g.setColour(p.white);
        g.fillRect(x - 1, 0, 3, getHeight());
    }
    const auto add = addButton();
    fillWithText(g, add, p.button, p.text, "+", juce::Justification::centred);
    const auto rest = getLocalBounds().withLeft(add.getRight() + 1);
    fillWithText(g, rest, p.bar, p.muted,
                 juce::String::fromUTF8("Thumbnail: trigger \xc2\xb7 name: select \xc2\xb7 drag from Files or the Finder"),
                 juce::Justification::centredRight, 8);
}

void DeckTabs::rename(uint64_t id)
{
    for (const auto& [area, tab] : tabs())
        if (tab == id)
            for (const Deck& deck : session_.composition().decks)
                if (deck.id == id)
                    editInline(editor_, *this, area, utf8(deck.name), [this, id](const juce::String& name) {
                        session_.client().edit(Rename{"deck", id, name.toStdString()});
                    });
}

void DeckTabs::mouseDoubleClick(const juce::MouseEvent& e)
{
    for (const auto& [area, id] : tabs())
        if (area.contains(e.getPosition()))
            rename(id);
}

// Duplicate puts a copy right after the deck, with fresh deck, column and effect ids.
Menu DeckTabs::deckMenu(uint64_t id)
{
    auto duplicate = [this, id] {
        const Composition& c = session_.composition();
        auto it = std::find_if(c.decks.begin(), c.decks.end(), [id](const Deck& d) { return d.id == id; });
        if (it == c.decks.end())
            return;
        uint64_t column = 0;
        for (const Deck& d : c.decks)
            column = std::max(column, maxId(d.columns));
        Deck copy{maxId(c.decks) + 1, it->name + " copy", {}};
        std::vector<Cell> cells;
        for (const Column& col : it->columns) {
            copy.columns.push_back({++column, col.name});
            forEachLayer(c.layers, [&](const Layer& layer) {
                if (auto clip = layer.clips.find(col.id); clip != layer.clips.end()) {
                    ClipRef fresh = clip->second;
                    for (Effect& effect : fresh.effects)
                        effect.id = 0;
                    cells.push_back({layer.id, column, fresh});
                }
            });
        }
        session_.client().edit(AddDeck{size_t(it - c.decks.begin()) + 1, copy, cells});
    };
    return {{.label = "Rename", .run = [this, id] { rename(id); }, .tip = "Name the deck (or double-click its tab)"},
            {.label = "Duplicate", .run = duplicate, .tip = "Add a copy of the deck right after it"},
            {.label = "Select all clips in this deck",
             .run = [this] { session_.selectClips(session_.deckClips()); },
             .enabled = id == session_.composition().deck,
             .shortcut = juce::String::fromUTF8("\xe2\x8c\x98" "A"),
             .tip = "Select every clip of the deck in view"},
            {},
            {.label = "Delete",
             .run = [this, id] { session_.client().edit(RemoveDeck{id}); },
             .enabled = session_.composition().decks.size() > 1,
             .tip = "Delete the deck with its columns and clips; the last deck stays"}};
}

size_t DeckTabs::gapAt(int x) const
{
    const auto all = tabs();
    for (size_t i = 0; i < all.size(); ++i)
        if (x < all[i].first.getCentreX())
            return i;
    return all.size();
}

void DeckTabs::moveTo(uint64_t deck, size_t gap)
{
    const std::vector<Deck>& decks = session_.composition().decks;
    auto it = std::find_if(decks.begin(), decks.end(), [deck](const Deck& d) { return d.id == deck; });
    if (it == decks.end())
        return;
    const size_t from = size_t(it - decks.begin());
    const size_t to = gap > from ? gap - 1 : gap;
    if (to != from)
        session_.client().edit(MoveDeck{deck, to});
}

void DeckTabs::mouseDown(const juce::MouseEvent& e)
{
    pressed_.reset();
    dropGap_.reset();
    if (e.mods.isPopupMenu())
        return;
    for (const auto& [area, id] : tabs())
        if (area.contains(e.getPosition()))
            pressed_ = id;
}

void DeckTabs::mouseDrag(const juce::MouseEvent& e)
{
    if (!pressed_ || (!dropGap_ && e.getDistanceFromDragStart() < 5))
        return;
    const size_t gap = gapAt(e.x);
    if (gap == dropGap_)
        return;
    dropGap_ = gap;
    repaint();
}

void DeckTabs::mouseUp(const juce::MouseEvent& e)
{
    const auto pressed = std::exchange(pressed_, std::nullopt);
    if (const auto gap = std::exchange(dropGap_, std::nullopt)) {
        repaint();
        return moveTo(*pressed, *gap);
    }
    for (const auto& [area, id] : tabs())
        if (area.contains(e.getPosition())) {
            if (e.mods.isPopupMenu())
                return showMenu(*this, deckMenu(id));
            return session_.client().selectDeck(id);
        }
    if (addButton().contains(e.getPosition())) {
        Deck deck{0, "Deck " + std::to_string(session_.composition().decks.size() + 1), {}};
        for (int i = 0; i < 4; ++i)
            deck.columns.push_back({0, "Column " + std::to_string(i + 1)});
        session_.client().edit(AddDeck{std::nullopt, deck, {}});
    }
}

juce::String DeckTabs::tipAt(juce::Point<int> point)
{
    for (const auto& [area, id] : tabs())
        if (area.contains(point))
            return badge(id).isNotEmpty()
                       ? "The column Autopilot steps through this deck's columns; set it in the Autopilot tab"
                       : "Show this deck's columns (live); drag to reorder, double-click renames, right-click "
                         "duplicates or deletes";
    if (addButton().contains(point))
        return "Add a deck with four empty columns";
    return {};
}

LayerStack::LayerStack(Session& session) : session_(session)
{
    session_.addView(this);
    rebuildRows();
}

LayerStack::~LayerStack()
{
    session_.removeView(this);
}

void LayerStack::sessionChanged(int changes)
{
    if (!(changes & ~(Session::Modulation | Session::Audio | Session::Previews)))
        return;
    if (const int active = activeSlot(); active != activeSlot_) {
        activeSlot_ = active;
        if (onActiveSlot && active >= 0)
            onActiveSlot(active);
    }
    if (!(changes & ~(Session::Clock | Session::Modulation | Session::Audio | Session::Previews)))
        return repaintLiveCells();
    if (changes & Session::Document) {
        rebuildRows();
        if (auto* grid = findParentComponentOfClass<ClipGrid>())
            grid->resized();
    }
    repaint();
}

void LayerStack::repaintLiveCells()
{
    std::set<std::pair<uint64_t, int>> live;
    for (const Row& row : rows_) {
        repaint(controlBounds(row));
        for (int slot = 0; !row.group && slot < slots(); ++slot)
            if (const auto column = columnAt(slot); column && (session_.client().playing(row.layer, *column) ||
                                                               session_.client().queued(row.layer, *column)))
                live.emplace(row.layer, slot);
    }
    live.insert(liveCells_.begin(), liveCells_.end());
    liveCells_.clear();
    for (const auto& [layer, slot] : live)
        if (const Row* row = findRow(layer))
            repaint(cellBounds(*row, slot));
}

void LayerStack::rebuildRows()
{
    rows_.clear();
    const auto& layers = session_.composition().layers;
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        rows_.push_back({it->id, it->group, {}, {}});
        for (auto inner = it->layers.rbegin(); inner != it->layers.rend(); ++inner)
            rows_.push_back({inner->id, inner->group, utf8(it->name), {}});
    }
    layout(width_);
}

int LayerStack::slots() const
{
    const Deck* deck = session_.shownDeck();
    return std::max(metrics().columns, deck ? int(deck->columns.size()) + 1 : 0);
}

int LayerStack::layout(int width)
{
    width_ = width;
    const Metrics& m = metrics();
    const int n = slots();
    tileWidth_ = std::max(m.cellWidth, (width - m.layerControlWidth - m.gap * (n + 2)) / n);
    contentWidth_ = std::max(width, m.layerControlWidth + m.gap * (n + 2) + tileWidth_ * n);
    rowHeight_ = std::max(kControlHeight, tileWidth_ * 9 / 16 + m.labelHeight);
    int y = 0;
    for (Row& row : rows_) {
        const int height = row.group ? kControlHeight : rowHeight_;
        row.bounds = {0, y, contentWidth_, height};
        y += height + 3;
    }
    addLayer_ = {m.gap + scrollX_, y, m.layerControlWidth, 20};
    return y + 24;
}

juce::Rectangle<int> LayerStack::columnArea(int slot) const
{
    const Metrics& m = metrics();
    const int x = m.gap + m.layerControlWidth + m.gap + slot * (tileWidth_ + m.gap);
    return {x, 0, tileWidth_, 0};
}

int LayerStack::activeSlot() const
{
    const Deck* deck = session_.shownDeck();
    if (!deck)
        return -1;
    const uint64_t stepped = session_.composition().columnAutopilot.mode != AutopilotMode::Off
                                 ? session_.client().clock().columns.column
                                 : 0;
    const uint64_t active = stepped ? stepped : session_.composition().column;
    for (size_t slot = 0; slot < deck->columns.size(); ++slot)
        if (deck->columns[slot].id == active)
            return int(slot);
    return -1;
}

void LayerStack::setScrollX(int x)
{
    if (x == scrollX_)
        return;
    scrollX_ = x;
    addLayer_.setX(metrics().gap + x);
    repaint();
}

juce::Rectangle<int> LayerStack::cellBounds(const Row& row, int slot) const
{
    const auto column = columnArea(slot);
    return {column.getX(), row.bounds.getY(), column.getWidth(), rowHeight_};
}

juce::Rectangle<int> LayerStack::controlBounds(const Row& row) const
{
    return {metrics().gap + scrollX_, row.bounds.getY(), metrics().layerControlWidth, kControlHeight};
}

const LayerStack::Row* LayerStack::findRow(uint64_t layer) const
{
    for (const Row& row : rows_)
        if (row.layer == layer)
            return &row;
    return nullptr;
}

std::optional<uint64_t> LayerStack::columnAt(int slot) const
{
    const Deck* deck = session_.shownDeck();
    if (!deck || slot < 0 || slot >= int(deck->columns.size()))
        return std::nullopt;
    return deck->columns[size_t(slot)].id;
}

const ClipRef* LayerStack::clipAt(const Row& row, int slot) const
{
    const Layer* layer = session_.layer(row.layer);
    const auto column = columnAt(slot);
    if (!layer || !column)
        return nullptr;
    auto it = layer->clips.find(*column);
    return it == layer->clips.end() ? nullptr : &it->second;
}

LayerStack::Hit LayerStack::partAt(juce::Point<int> point) const
{
    if (addLayer_.contains(point))
        return {Part::AddLayer};
    for (const Row& row : rows_) {
        if (!row.bounds.contains(point))
            continue;
        const auto control = controlBounds(row);
        if (control.contains(point)) {
            const int line = (point.y - control.getY()) / (kControlLine + 1);
            if (line == 0) {
                if (point.x >= control.getRight() - kButtonWidth)
                    return {Part::Solo, row.layer};
                if (point.x >= control.getRight() - 2 * kButtonWidth - 1)
                    return {Part::Bypass, row.layer};
                return {Part::Name, row.layer};
            }
            if (line == 1) {
                if (!row.group && point.x >= control.getRight() - 72)
                    return {Part::Auto, row.layer};
                return {Part::Blend, row.layer};
            }
            return {Part::Opacity, row.layer};
        }
        if (row.group)
            return {Part::None, row.layer};
        for (int slot = 0; slot < slots(); ++slot) {
            const auto cell = cellBounds(row, slot);
            if (cell.contains(point))
                return {point.y >= cell.getBottom() - metrics().labelHeight ? Part::Label : Part::Thumb, row.layer, slot};
        }
    }
    return {};
}

void LayerStack::paint(juce::Graphics& g)
{
    const Palette& p = palette();
    g.fillAll(p.background);
    g.setFont(Look::font());
    for (const Row& row : rows_) {
        if (!g.clipRegionIntersects(row.bounds))
            continue;
        if (row.group) {
            const auto strip = juce::Rectangle<int>(columnArea(0).getX(), row.bounds.getY(),
                                                    columnArea(slots() - 1).getRight() - columnArea(0).getX(),
                                                    kControlHeight);
            fillWithText(g, strip, p.header, p.muted, "group", juce::Justification::centredLeft, 8);
            continue;
        }
        for (int slot = 0; slot < slots(); ++slot)
            if (g.clipRegionIntersects(cellBounds(row, slot)))
                paintCell(g, row, slot);
    }
    // The controls stay pinned at the left while the cells scroll under them.
    if (scrollX_ > 0) {
        g.setColour(p.background);
        g.fillRect(scrollX_, 0, metrics().gap * 2 + metrics().layerControlWidth, getHeight());
    }
    for (const Row& row : rows_)
        if (g.clipRegionIntersects(row.bounds))
            paintControl(g, row);
    fillWithText(g, addLayer_, p.header, p.muted, "+ Layer");
}

void LayerStack::paintControl(juce::Graphics& g, const Row& row)
{
    const Layer* layer = session_.layer(row.layer);
    if (!layer)
        return;
    const Palette& p = palette();
    const auto area = controlBounds(row);
    g.setColour(p.cell);
    g.fillRect(area);
    const bool selected = session_.selectedLayer() == row.layer;

    auto name = area.withHeight(kControlLine);
    auto solo = name.removeFromRight(kButtonWidth);
    name.removeFromRight(1);
    auto bypass = name.removeFromRight(kButtonWidth);
    name.removeFromRight(1);
    const juce::Colour nameFill = selected ? p.live : row.group ? p.groupLabel : p.clipLabel;
    if (row.groupName.isNotEmpty()) {
        g.setColour(p.groupLabel);
        g.fillRect(name.removeFromLeft(4));
    }
    fillWithText(g, name, nameFill, selected ? p.liveInk : p.white, utf8(layer->name));
    g.setFont(Look::font(true));
    fillWithText(g, bypass, layer->bypass ? p.accent : p.button, layer->bypass ? p.liveInk : p.text, "B",
                 juce::Justification::centred, 0);
    fillWithText(g, solo, layer->solo ? p.live : p.button, layer->solo ? p.liveInk : p.text, "S",
                 juce::Justification::centred, 0);
    g.setFont(Look::font());

    auto line = area.withY(area.getY() + kControlLine + 1).withHeight(kControlLine).reduced(5, 0);
    g.setColour(p.text);
    const juce::String blend = blendModeName(deckBlend(*layer, session_.composition().deck));
    drawText(g, blend, line, juce::Justification::centredLeft);
    const int blendWidth = juce::GlyphArrangement::getStringWidthInt(Look::font(), blend);
    drawChevron(g, line.withTrimmedLeft(blendWidth).withWidth(line.getHeight()), true, p.text);
    if (!row.group) {
        const juce::String label = autopilotLabel(row.layer);
        const bool on = deckAutopilot(*layer, session_.composition().deck).mode != AutopilotMode::Off;
        g.setColour(label.startsWith("manual") ? p.live : on ? p.accent : p.muted);
        drawText(g, label, line, juce::Justification::centredRight);
    }

    auto opacity = area.withY(area.getY() + 2 * (kControlLine + 1)).withHeight(kControlLine).reduced(5, 2);
    auto value = opacity.removeFromRight(36);
    double shown = layer->opacity;
    if (dragOpacity_ && dragOpacity_->first == row.layer)
        shown = dragOpacity_->second;
    drawBar(g, opacity, float(shown), p.meter);
    g.setColour(p.text);
    drawText(g, juce::String(juce::roundToInt(shown * 100.0)) + "%", value, juce::Justification::centredRight);
    if (dropTarget_ && dropTarget_->first == row.layer && dropTarget_->second < 0) {
        g.setColour(p.white);
        g.drawRect(area, 2);
    }
}

void LayerStack::paintCell(juce::Graphics& g, const Row& row, int slot)
{
    const Palette& p = palette();
    const auto cell = cellBounds(row, slot);
    auto thumb = cell.withHeight(cell.getHeight() - metrics().labelHeight);
    const auto label = cell.withTop(thumb.getBottom());
    const ClipRef* clip = clipAt(row, slot);
    const auto column = columnAt(slot);
    if (!clip) {
        g.setColour(p.emptyCell);
        g.fillRect(thumb);
        g.setColour(juce::Colour(0xff1a1a1a));
        g.fillRect(label);
    } else {
        const bool failed = session_.client().failedClips().contains(clip->hash);
        const bool playing = session_.client().playing(row.layer, *column);
        const bool queued = session_.client().queued(row.layer, *column);
        if (playing || queued)
            liveCells_.emplace(row.layer, slot);
        const bool selected = session_.isSelected({row.layer, *column}) ||
                              session_.selectedClip() == Session::ClipCell{row.layer, *column};

        g.setColour(juce::Colours::black);
        g.fillRect(thumb);
        const bool generator = isGeneratorClip(*clip);
        if (generator)
            paintGenerator(g, thumb, clip->effects.front());
        else if (juce::Image still = session_.still(clip->hash); still.isValid())
            g.drawImage(still, thumb.toFloat(), juce::RectanglePlacement::fillDestination);
        if (failed) {
            g.setColour(juce::Colour(0xb03a1414));
            g.fillRect(thumb);
        }

        juce::Colour fill = p.clipLabel, ink = p.white;
        if (selected)
            fill = p.selectedLabel, ink = juce::Colours::black;
        if (playing)
            fill = p.live, ink = p.liveInk;
        if (failed)
            fill = p.fault, ink = juce::Colours::white;
        fillWithText(g, label, fill, ink, utf8(clip->name));
        if (const size_t effects = clip->effects.size() - (generator ? 1 : 0); effects > 0) {
            g.setFont(Look::smallFont());
            const juce::String fx = "fx " + juce::String(int(effects));
            const auto badge = juce::Rectangle<int>(juce::GlyphArrangement::getStringWidthInt(Look::smallFont(), fx) + 6, 13)
                                   .withPosition(thumb.getRight() - 3, thumb.getY() + 3)
                                   .translated(-(juce::GlyphArrangement::getStringWidthInt(Look::smallFont(), fx) + 6), 0);
            fillWithText(g, badge, juce::Colour(0xcc000000), p.live, fx, juce::Justification::centred, 0);
            g.setFont(Look::font());
        }

        if (playing) {
            const ClockState clock = session_.client().clock();
            const Layer* layer = session_.layer(row.layer);
            auto shown = clock.clips.find(row.layer);
            if (layer && layer->playing && layer->playing->beat && shown != clock.clips.end() && shown->second.beats > 0) {
                const double beats = session_.client().beatNow() - *layer->playing->beat;
                const double progress = std::fmod(std::max(0.0, beats), double(shown->second.beats)) / shown->second.beats;
                g.setColour(p.live);
                g.fillRect(thumb.getX(), thumb.getBottom() - 2, juce::roundToInt(thumb.getWidth() * progress), 2);
            }
            g.drawRect(cell, 2);
        }
        if (selected && !playing) {
            g.setColour(p.selectedLabel);
            g.drawRect(cell, 1);
        }
        if (queued && std::fmod(seconds(), 0.94) < 0.47) {
            g.setColour(p.accent);
            g.drawRect(cell, 2);
        }
    }
    if (dropTarget_ && dropTarget_->first == row.layer && dropTarget_->second == slot) {
        g.setColour(p.white);
        g.drawRect(cell, 2);
    }
}

void LayerStack::mouseDown(const juce::MouseEvent& e)
{
    pressed_ = partAt(e.getPosition());
    dragging_ = false;
    collapse_ = false;
    const Row* row = findRow(pressed_.layer);
    if (e.mods.isPopupMenu()) {
        const Hit hit = pressed_;
        pressed_ = {};
        if (hit.part == Part::Thumb || hit.part == Part::Label) {
            if (row && clipAt(*row, hit.slot)) {
                if (!session_.isSelected({hit.layer, *columnAt(hit.slot)}))
                    session_.selectClip({hit.layer, *columnAt(hit.slot)});
                showMenu(*this, cellMenu(hit.layer, hit.slot));
            }
        } else if (hit.layer && hit.part != Part::None) {
            showMenu(*this, layerMenu(hit.layer));
        }
        return;
    }
    switch (pressed_.part) {
    case Part::Name:
        if (e.mods.isShiftDown())
            session_.selectClips(session_.deckClips(pressed_.layer));
        else
            session_.selectLayer(pressed_.layer);
        break;
    case Part::Bypass:
        if (const Layer* layer = session_.layer(pressed_.layer))
            session_.client().edit(SetLayerBypass{layer->id, !layer->bypass});
        break;
    case Part::Solo:
        if (const Layer* layer = session_.layer(pressed_.layer))
            session_.client().edit(SetLayerSolo{layer->id, !layer->solo});
        break;
    case Part::Blend:
        if (row)
            showBlendMenu(*row);
        break;
    case Part::Auto:
        if (row)
            showAutopilotMenu(*row);
        break;
    case Part::Opacity:
        if (row) {
            session_.client().beginGesture();
            lastOpacitySend_ = 0.0;
            setOpacity(*row, e.x);
        }
        break;
    case Part::Thumb:
    case Part::Label:
        if (row && columnAt(pressed_.slot) && (clipAt(*row, pressed_.slot) || e.mods.isShiftDown()))
            pressCell({pressed_.layer, *columnAt(pressed_.slot)}, e.mods);
        break;
    case Part::AddLayer: {
        Layer layer;
        layer.name = "Layer " + std::to_string(rows_.size() + 1);
        session_.client().edit(AddLayer{0, std::nullopt, layer});
        break;
    }
    case Part::None: break;
    }
}

// Cmd toggles the cell, Shift selects the block from the last one; a plain press on the name selects it alone, or on
// release when it is part of a block, which it may drag. A selecting press on the thumbnail does not trigger.
void LayerStack::pressCell(Session::ClipCell cell, juce::ModifierKeys mods)
{
    if (mods.isCommandDown() || mods.isShiftDown()) {
        pressed_.part = Part::Label;
        return mods.isCommandDown() ? session_.toggleClip(cell) : session_.selectRange(cell);
    }
    if (pressed_.part == Part::Thumb)
        return;
    if (session_.isSelected(cell) && session_.selectedClips().size() > 1)
        collapse_ = true;
    else
        session_.selectClip(cell);
}

void LayerStack::mouseDrag(const juce::MouseEvent& e)
{
    const Row* row = findRow(pressed_.layer);
    if (pressed_.part == Part::Opacity && row) {
        setOpacity(*row, e.x);
        return;
    }
    if (pressed_.part == Part::Name && row && !dragging_ && e.getDistanceFromDragStart() >= 5) {
        dragging_ = true;
        startDrag(*this, "layer:" + juce::String(int64_t(row->layer)), controlBounds(*row), e);
        return;
    }
    if (dragging_ || !row || (pressed_.part != Part::Thumb && pressed_.part != Part::Label) ||
        e.getDistanceFromDragStart() < 5 || !clipAt(*row, pressed_.slot))
        return;
    dragging_ = true;
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this)) {
        const juce::String description = "cell:" + juce::String(int64_t(pressed_.layer)) + ":" + juce::String(pressed_.slot);
        juce::Image still = session_.still(clipAt(*row, pressed_.slot)->hash);
        juce::ScaledImage image(still.isValid() ? still.rescaled(tileWidth_, tileWidth_ * 9 / 16) : juce::Image());
        container->startDragging(description, this, image);
    }
}

void LayerStack::mouseUp(const juce::MouseEvent&)
{
    const Row* row = findRow(pressed_.layer);
    if (pressed_.part == Part::Opacity && dragOpacity_ && row) {
        session_.client().edit(SetLayerOpacity{row->layer, dragOpacity_->second});
        session_.client().endGesture();
        dragOpacity_.reset();
    }
    if (pressed_.part == Part::Thumb && !dragging_ && row && clipAt(*row, pressed_.slot))
        session_.clickClip({pressed_.layer, *columnAt(pressed_.slot)});
    if (collapse_ && !dragging_ && row && clipAt(*row, pressed_.slot))
        session_.selectClip({pressed_.layer, *columnAt(pressed_.slot)});
    collapse_ = false;
    pressed_ = {};
    dragging_ = false;
}

juce::String LayerStack::tipAt(juce::Point<int> point)
{
    const Hit hit = partAt(point);
    const Row* row = findRow(hit.layer);
    const bool clip = row && hit.slot >= 0 && clipAt(*row, hit.slot);
    switch (hit.part) {
    case Part::Name:
        return "Select the layer (Shift: all its clips); drag to reorder, double-click renames, right-click for more";
    case Part::Bypass: return "Bypass: hide this layer";
    case Part::Solo: return "Solo: show only this layer";
    case Part::Blend: return "How this layer blends onto the layers below it";
    case Part::Auto: return "Autopilot: let this layer trigger its own clips, or back to manual";
    case Part::Opacity: return "Layer opacity, 0 to 100%: drag sideways";
    case Part::AddLayer: return "Add a layer on top";
    case Part::Thumb:
        return clip ? "Click to trigger; Cmd-click adds to the selection, Shift-click selects a block; drag onto "
                      "another cell to move (Option: copy), right-click for more"
                    : "Drop a clip, a folder or media files here";
    case Part::Label:
        return clip ? "Select the clip to edit it in the Clip tab without triggering it; Cmd-click adds, Shift-click "
                      "selects a block, Cmd-A every clip of the deck"
                    : "Drop a clip here";
    case Part::None: break;
    }
    return {};
}

void LayerStack::mouseDoubleClick(const juce::MouseEvent& e)
{
    const Hit hit = partAt(e.getPosition());
    if (hit.part == Part::Name)
        renameLayer(hit.layer);
}

void LayerStack::renameLayer(uint64_t id)
{
    const Row* row = findRow(id);
    const Layer* layer = session_.layer(id);
    if (!row || !layer)
        return;
    editInline(editor_, *this, controlBounds(*row).withHeight(kControlLine).withTrimmedRight(2 * kButtonWidth + 2),
               utf8(layer->name),
               [this, id](const juce::String& name) { session_.client().edit(Rename{"layer", id, name.toStdString()}); });
}

juce::Rectangle<int> LayerStack::controlArea(uint64_t layer) const
{
    const Row* row = findRow(layer);
    return row ? controlBounds(*row) : juce::Rectangle<int>();
}

juce::String LayerStack::autopilotLabel(uint64_t layer) const
{
    const ClockState clock = session_.client().clock();
    if (auto shown = clock.clips.find(layer); shown != clock.clips.end() && shown->second.held > 0)
        return "manual " + juce::String(shown->second.held);
    const Layer* l = session_.layer(layer);
    if (!l)
        return {};
    const Composition& composition = session_.composition();
    if (composition.columnAutopilot.mode != AutopilotMode::Off &&
        deckAutopilot(*l, composition.deck).mode != AutopilotMode::Off)
        return "auto paused";
    return autopilotText(*l, composition.deck);
}

Menu LayerStack::layerMenu(uint64_t id)
{
    const Composition& composition = session_.composition();
    const Layer* layer = session_.layer(id);
    if (!layer)
        return {};
    const auto [parent, index] = placeOf(composition, id);
    const size_t count = siblings(composition, parent).size();
    EngineClient& client = session_.client();
    Menu menu{{.label = "Rename", .run = [this, id] { renameLayer(id); }, .tip = "Name the layer (or double-click its name)"}};
    if (!layer->group)
        menu.push_back({.label = "Stop layer",
                        .run = [&client, id] { client.stopLayer(id); },
                        .enabled = layer->playing.has_value(),
                        .tip = "The layer plays nothing until its next trigger (live)"});
    if (!layer->group) {
        Menu groups;
        for (const Layer& g : composition.layers)
            if (g.group)
                groups.push_back({.label = utf8(g.name),
                                  .run = [&client, id, group = g.id, size = g.layers.size()] {
                                      client.edit(MoveLayer{id, group, size});
                                  },
                                  .ticked = parent == g.id,
                                  .tip = "Move the layer into this group"});
        if (!groups.empty()) {
            groups.push_back({.label = "None",
                              .run = [&client, id, top = composition.layers.size()] { client.edit(MoveLayer{id, 0, top}); },
                              .ticked = parent == 0,
                              .tip = "Take the layer out of its group"});
            menu.push_back({.label = "Group", .sub = groups, .tip = "Put the layer into a group, or take it out"});
        }
    }
    menu.push_back({.label = "Move up",
                    .run = [&client, id, parent, index] { client.edit(MoveLayer{id, parent, index + 1}); },
                    .enabled = index + 1 < count,
                    .tip = "Move the layer one place up"});
    menu.push_back({.label = "Move down",
                    .run = [&client, id, parent, index] { client.edit(MoveLayer{id, parent, index - 1}); },
                    .enabled = index > 0,
                    .tip = "Move the layer one place down"});
    Session& session = session_;
    menu.push_back({});
    if (!layer->group)
        menu.push_back({.label = "Select all clips in layer",
                        .run = [&session, id] { session.selectClips(session.deckClips(id)); },
                        .shortcut = juce::String::fromUTF8("\xe2\x87\xa7-click"),
                        .tip = "Select every clip of this layer in the deck in view"});
    menu.push_back({.label = "Select all clips in this deck",
                    .run = [&session] { session.selectClips(session.deckClips()); },
                    .shortcut = juce::String::fromUTF8("\xe2\x8c\x98" "A"),
                    .tip = "Select every clip of the deck in view"});
    menu.push_back({});
    menu.push_back({.label = "Add effect",
                    .sub = addEffectMenu(session_,
                                         [&session, id](const Effect& effect) {
                                             session.client().edit(
                                                 addEffectTo(session.composition(), {id}, std::nullopt, effect));
                                         }),
                    .tip = "Add an effect at the end of the layer's chain"});
    menu.push_back({.label = "Copy effects",
                    .run = [this, id] { copyEffects({id}); },
                    .enabled = !deckEffects(*layer, composition.deck).empty(),
                    .tip = "Copy the layer's effects with their settings"});
    menu.push_back({.label = "Paste effects",
                    .run = [this, id] { pasteEffects({id}); },
                    .enabled = !session_.copiedEffects.empty(),
                    .tip = "Add the copied effects at the end of the layer's chain"});
    if (!layer->group)
        menu.push_back({.label = "Move clip effects to layer",
                        .run = [this, id] { hoistClipEffects(id); },
                        .tip = "The effects the layer's selected clips (or all its clips) carry alike move to the "
                               "front of the layer's chain, or of each deck's chain where they differ by deck"});
    menu.push_back({});
    menu.push_back({.label = layer->group ? "Delete group" : "Delete layer",
                    .run = [&client, id] { client.edit(RemoveLayer{id}); },
                    .tip = layer->group ? "Delete the group with its layers" : "Delete the layer with its clips"});
    return menu;
}

// Ticked when every clip has it, marked mixed when some do.
static MenuEntry stateEntry(const juce::String& label, const juce::String& tip, const std::vector<const ClipRef*>& clips,
                            const std::function<bool(const ClipRef&)>& has, std::function<void()> run)
{
    const auto count = std::count_if(clips.begin(), clips.end(), [&](const ClipRef* c) { return has(*c); });
    return {.label = label,
            .run = std::move(run),
            .ticked = count > 0 && count == std::ssize(clips),
            .tip = tip,
            .mixed = count > 0 && count < std::ssize(clips)};
}

static juce::String plural(int n, const char* unit)
{
    return juce::String(n) + " " + unit + (n == 1 ? "" : "s");
}

// Everything acts on every selected clip when the cell is one of them, else on its clip alone.
Menu LayerStack::cellMenu(uint64_t layer, int slot)
{
    const auto column = columnAt(slot);
    if (!column || !session_.clip({layer, *column}))
        return {};
    const Session::ClipCell cell{layer, *column};
    const std::vector<Session::ClipCell> cells = session_.clipTargets(cell);
    std::vector<const ClipRef*> clips;
    for (const Session::ClipCell& c : cells)
        clips.push_back(session_.clip(c));
    Session& session = session_;
    auto edit = [&session, cell](const juce::String& name, std::function<Edit(Session::ClipCell)> make) {
        return [&session, cell, name, make] {
            session.editClips(cell, {name, [make](Session::ClipCell c, const ClipRef&) { return std::vector<Edit>{make(c)}; }});
        };
    };
    const bool several = cells.size() > 1;
    Menu menu{
        {.label = "Trigger",
         .run = [&session, cells] {
             for (const Session::ClipCell& c : cells)
                 session.client().triggerClip(c.layer, c.column);
         },
         .tip = several ? "Trigger the selected clips now (live)" : "Trigger this clip now (live)"},
        {.label = "Select", .run = [&session, cell] { session.selectClip(cell); }, .tip = "Select this clip alone"},
        {.label = "Select all clips in layer",
         .run = [&session, layer] { session.selectClips(session.deckClips(layer)); },
         .shortcut = juce::String::fromUTF8("\xe2\x87\xa7-click layer"),
         .tip = "Select every clip of this layer in the deck in view"},
        {.label = "Select all clips in column",
         .run = [&session, column = *column] { session.selectClips(session.deckClips(std::nullopt, column)); },
         .tip = "Select every clip of this column"},
        {},
        {.label = "Copy",
         .run = [this] { copyClips(); },
         .shortcut = juce::String::fromUTF8("\xe2\x8c\x98" "C"),
         .tip = "Copy the selected clips to paste them elsewhere in the grid"},
        {.label = "Paste",
         .run = [this, cell] { pasteClips(cell); },
         .enabled = !session_.copiedClips.empty(),
         .shortcut = juce::String::fromUTF8("\xe2\x8c\x98V"),
         .tip = "Paste the copied clips with their top left here, over what is there"},
        {.label = "Duplicate",
         .run = [this] { duplicateClips(); },
         .shortcut = juce::String::fromUTF8("\xe2\x8c\x98" "D"),
         .tip = "Copy the selected clips into the cells right of them"},
        {.label = "Clear",
         .run = [this] { removeSelectedClip(); },
         .shortcut = juce::String::fromUTF8("\xe2\x8c\xab"),
         .tip = several ? "Empty the selected clips' cells" : "Empty this cell"},
        {},
        {.label = "Add effect",
         .sub = addEffectMenu(session_, [&session, cell](const Effect& effect) {
             session.editClips(cell, Session::addingEffect(effect));
         }),
         .tip = "Add an effect at the end of each selected clip's chain"},
        {.label = "Copy effects",
         .run = [this, cell] { copyEffects({cell.layer, cell.column}); },
         .enabled = !clips.front()->effects.empty(),
         .shortcut = juce::String::fromUTF8("\xe2\x87\xa7\xe2\x8c\x98" "C"),
         .tip = "Copy this clip's effects with their settings"},
        {.label = "Paste effects",
         .run = [this, cell] { pasteEffects({cell.layer, cell.column}); },
         .enabled = !session_.copiedEffects.empty(),
         .shortcut = juce::String::fromUTF8("\xe2\x87\xa7\xe2\x8c\x98V"),
         .tip = "Add the copied effects at the end of each selected clip's chain"}};

    auto play = [&](const juce::String& label, const juce::String& tip, PlayMode mode) {
        return stateEntry(label, tip, clips, [mode](const ClipRef& c) { return c.transport.playMode == mode; },
                          edit("Play " + label, [mode](Session::ClipCell c) {
                              return SetClipTransport{.layer = c.layer, .playMode = mode, .column = c.column};
                          }));
    };
    const bool allBpm =
        std::all_of(clips.begin(), clips.end(), [](const ClipRef* c) { return c->transport.sync == SyncMode::Bpm; });
    const SyncMode sync = allBpm ? SyncMode::Timeline : SyncMode::Bpm;
    Menu transport{play("Loop", "Play from in to out, round and round", PlayMode::Loop),
                   play("Bounce", "Play forward, then backward", PlayMode::Bounce),
                   play("Random Jump", "Loop and jump on the clip's beat grid", PlayMode::RandomJump),
                   {},
                   stateEntry("BPM sync", "On: the clip stretches to its length in beats; off: its own frame rate", clips,
                              [](const ClipRef& c) { return c.transport.sync == SyncMode::Bpm; },
                              edit("Sync", [sync](Session::ClipCell c) {
                                  return SetClipTransport{.layer = c.layer, .sync = sync, .column = c.column};
                              }))};
    Menu steps{stateEntry("Layer's", "Autopilot plays the clip for the layer's Every", clips,
                          [](const ClipRef& c) { return c.step == 0; },
                          edit("Step", [](Session::ClipCell c) { return SetClipStep{c.layer, c.column, 0}; }))};
    for (int bars : {1, 2, 4, 8, 16, 32, 64})
        steps.push_back(stateEntry(plural(bars, "bar"), "Autopilot plays the clip for " + plural(bars, "bar"), clips,
                                   [bars](const ClipRef& c) { return c.step == bars; },
                                   edit("Step", [bars](Session::ClipCell c) { return SetClipStep{c.layer, c.column, bars}; })));
    Menu transitions{stateEntry("Layer's", "Come in over the layer's transition length", clips,
                                [](const ClipRef& c) { return !c.transition; },
                                edit("Transition", [](Session::ClipCell c) {
                                    return SetClipTransition{c.layer, c.column, std::nullopt};
                                }))};
    for (int beats : {0, 1, 2, 4, 8, 16})
        transitions.push_back(stateEntry(beats ? plural(beats, "beat") : juce::String("Cut"),
                                         beats ? "Come in over " + plural(beats, "beat") : juce::String("Switch at once"),
                                         clips, [beats](const ClipRef& c) { return c.transition == double(beats); },
                                         edit("Transition", [beats](Session::ClipCell c) {
                                             return SetClipTransition{c.layer, c.column, double(beats)};
                                         })));
    std::vector<std::string> types;
    for (const ClipRef* c : clips)
        for (const Effect& effect : c->effects)
            if (std::find(types.begin(), types.end(), effect.type) == types.end())
                types.push_back(effect.type);
    Menu removals;
    for (const std::string& type : types) {
        const auto holding = std::count_if(clips.begin(), clips.end(), [&](const ClipRef* c) {
            return std::any_of(c->effects.begin(), c->effects.end(), [&](const Effect& e) { return e.type == type; });
        });
        removals.push_back({.label = utf8(type),
                            .run = [&session, cell, type] { session.editClips(cell, Session::removingEffects(type)); },
                            .tip = "Remove every " + utf8(type) + " from the selected clips",
                            .mixed = holding < std::ssize(clips)});
    }
    if (!removals.empty())
        menu.push_back({.label = "Remove effect", .sub = removals,
                        .tip = "Remove an effect type from every selected clip that has it"});
    menu.push_back({});
    menu.push_back({.label = "Transport", .sub = transport, .tip = "Play mode and BPM sync of the selected clips"});
    menu.push_back({.label = "Step", .sub = steps, .tip = "How long Autopilot plays the selected clips"});
    menu.push_back({.label = "Transition", .sub = transitions, .tip = "How the selected clips come in"});
    menu.push_back({});
    menu.push_back({.label = juce::String::fromUTF8("Rename\xe2\x80\xa6"),
                    .run = [&session, cells, name = utf8(clips.front()->name)] {
                        askName("Rename", cells.size() > 1 ? "A name for the selected clips" : "A name for the clip", name,
                                [&session, cells](const juce::String& text) {
                                    std::vector<Edit> edits;
                                    for (const Session::ClipCell& c : cells)
                                        if (const ClipRef* clip = session.clip(c)) {
                                            ClipRef renamed = *clip;
                                            renamed.name = text.toStdString();
                                            edits.push_back(SetSlot{c.layer, c.column, renamed});
                                        }
                                    session.edit(std::move(edits));
                                });
                    },
                    .tip = "Name the selected clips"});
    std::set<std::string> missing;
    for (const ClipRef* clip : clips)
        if (session_.client().failedClips().contains(clip->hash))
            missing.insert(clip->hash);
    const auto& last = session_.lastClipEdit();
    menu.push_back({.label = "Repeat last edit",
                    .run = [&session] { session.repeatLastEdit(); },
                    .enabled = last.has_value(),
                    .shortcut = juce::String::fromUTF8("\xe2\x8c\x98R"),
                    .tip = last ? "Repeat " + last->name + " on the selected clips; clips without that effect are skipped"
                                : juce::String("Repeat the last clip edit on the selected clips")});
    if (!missing.empty()) {
        menu.push_back({.label = "Relink (find in the Media Library by hash)",
                        .run = [&session, missing] {
                            for (const std::string& hash : missing)
                                session.client().relink(hash);
                        },
                        .tip = "Find the missing media again in the Media Library's folders by its content"});
        if (missing.size() == 1)
            menu.push_back({.label = juce::String::fromUTF8("Relink to a file\xe2\x80\xa6"),
                            .run = [this, hash = *missing.begin()] { relinkToFile(hash); },
                            .tip = "Point the missing media at a file on the engine machine"});
    }
    return menu;
}

// A file on the engine machine: chosen here for a local engine, typed for a remote one.
void LayerStack::relinkToFile(const std::string& hash)
{
    auto use = [this, hash](const juce::String& path) {
        if (path.isNotEmpty())
            session_.client().relink(hash, path.toStdString());
    };
    if (!session_.client().local())
        return askName("Relink", "A path on the engine machine", "", use);
    chooser_ = std::make_unique<juce::FileChooser>("Relink the missing media",
                                                   juce::File::getSpecialLocation(juce::File::userHomeDirectory));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [use](const juce::FileChooser& chooser) {
                              if (chooser.getResult() != juce::File())
                                  use(chooser.getResult().getFullPathName());
                          });
}

void LayerStack::removeSelectedClip()
{
    std::vector<Edit> edits;
    if (const auto cell = session_.selectedClip())
        for (const Session::ClipCell& c : session_.clipTargets(*cell))
            if (session_.clip(c))
                edits.push_back(SetSlot{c.layer, c.column, std::nullopt});
    session_.edit(std::move(edits));
}

std::vector<Session::ClipCell> LayerStack::targets() const
{
    const auto cell = session_.selectedClip();
    return cell ? session_.clipTargets(*cell) : std::vector<Session::ClipCell>{};
}

// The selected clips of the deck in view by rows and columns from `origin`.
std::vector<Session::CopiedClip> LayerStack::block(Session::ClipCell origin) const
{
    std::vector<Session::CopiedClip> clips;
    const auto from = session_.gridPosition(origin);
    for (const Session::ClipCell& cell : session_.clipTargets(origin)) {
        const auto at = session_.gridPosition(cell);
        if (from && at && session_.clip(cell))
            clips.push_back({at->first - from->first, at->second - from->second, *session_.clip(cell)});
    }
    return clips;
}

// After `first`, puts the clips at their offsets from `at` as one undo step and selects them; with `fresh` their
// effects get new ids. Nothing happens when part of the block would leave the grid.
void LayerStack::placeBlock(const std::vector<Session::CopiedClip>& clips, Session::ClipCell at, std::vector<Edit> first,
                            bool fresh)
{
    std::vector<Session::ClipCell> placed;
    for (const Session::CopiedClip& copied : clips) {
        const auto cell = session_.offsetCell(at, copied.row, copied.column);
        if (!cell)
            return session_.showStatus("The clips do not fit there", true);
        ClipRef clip = copied.clip;
        if (fresh)
            for (Effect& effect : clip.effects)
                effect.id = 0;
        first.push_back(SetSlot{cell->layer, cell->column, clip});
        placed.push_back(*cell);
    }
    session_.edit(std::move(first));
    session_.selectClips(placed);
}

// A move keeps the clips' effect ids, so macros and modulators on them follow; a copy gets fresh ones.
void LayerStack::moveBlock(Session::ClipCell from, Session::ClipCell to, bool copy)
{
    std::vector<Edit> clears;
    if (!copy)
        for (const Session::ClipCell& cell : session_.clipTargets(from))
            if (session_.gridPosition(cell))
                clears.push_back(SetSlot{cell.layer, cell.column, std::nullopt});
    placeBlock(block(from), to, std::move(clears), copy);
}

// The block's top left cell in the grid.
static std::optional<Session::ClipCell> topLeft(const Session& session, const std::vector<Session::ClipCell>& cells)
{
    std::optional<std::pair<int, int>> corner;
    for (const Session::ClipCell& cell : cells)
        if (const auto at = session.gridPosition(cell))
            corner = corner ? std::make_pair(std::min(corner->first, at->first), std::min(corner->second, at->second)) : *at;
    if (!corner || cells.empty())
        return std::nullopt;
    const auto first = session.gridPosition(cells.front());
    return session.offsetCell(cells.front(), corner->first - first->first, corner->second - first->second);
}

void LayerStack::copyClips()
{
    const auto cells = targets();
    if (const auto corner = topLeft(session_, cells))
        session_.copiedClips = block(*corner);
}

void LayerStack::pasteClips(std::optional<Session::ClipCell> at)
{
    if (!at)
        at = topLeft(session_, targets());
    if (at && !session_.copiedClips.empty())
        placeBlock(session_.copiedClips, *at, {}, true);
}

// Copies right of the block, as wide as it is.
void LayerStack::duplicateClips()
{
    const auto cells = targets();
    const auto corner = topLeft(session_, cells);
    if (!corner)
        return;
    const std::vector<Session::CopiedClip> clips = block(*corner);
    int width = 0;
    for (const Session::CopiedClip& c : clips)
        width = std::max(width, c.column + 1);
    if (const auto at = session_.offsetCell(*corner, 0, width))
        placeBlock(clips, *at, {}, true);
    else
        session_.showStatus("The clips do not fit there", true);
}

void LayerStack::copyEffects(const ChainOwner& owner)
{
    if (const auto* chain = chainOf(session_.composition(), owner))
        session_.copiedEffects = *chain;
}

// Onto every selected clip when the owner is one of them.
void LayerStack::pasteEffects(const ChainOwner& owner)
{
    std::vector<Effect> effects = session_.copiedEffects;
    for (Effect& effect : effects)
        effect.id = 0;
    if (effects.empty())
        return;
    if (owner.column)
        return session_.editClips({owner.layer, *owner.column},
                                  {"paste effects", [effects](Session::ClipCell c, const ClipRef&) {
                                       std::vector<Edit> edits;
                                       for (const Effect& effect : effects)
                                           edits.push_back(AddEffect{c.layer, std::nullopt, effect, c.column});
                                       return edits;
                                   }});
    std::vector<Edit> edits;
    for (const Effect& effect : effects)
        edits.push_back(addEffectTo(session_.composition(), owner, std::nullopt, effect));
    session_.edit(std::move(edits));
}

// The importer's hoist: a chain alike on every clip goes to the layer's shared chain, keeping the first clip's effect ids
// (with one deck, or clips in several); else each deck whose clips carry one alike gets it ahead of its own chain (or
// copies of the shared one). One undo step.
void LayerStack::hoistClipEffects(uint64_t id)
{
    const Composition& composition = session_.composition();
    const Layer* layer = session_.layer(id);
    if (!layer)
        return;
    std::vector<Session::ClipCell> cells;
    for (const Session::ClipCell& cell : session_.selectedClips())
        if (cell.layer == id && session_.clip(cell))
            cells.push_back(cell);
    if (cells.empty())
        for (const auto& [column, clip] : layer->clips)
            cells.push_back({id, column});
    auto alike = [&](const std::vector<Session::ClipCell>& group) {
        const std::vector<Effect>& chain = session_.clip(group.front())->effects;
        return !chain.empty() && std::all_of(group.begin(), group.end(), [&](const Session::ClipCell& c) {
            return std::equal(chain.begin(), chain.end(), session_.clip(c)->effects.begin(), session_.clip(c)->effects.end(),
                              [](const Effect& a, const Effect& b) {
                                  return a.type == b.type && a.params == b.params && a.bypass == b.bypass;
                              });
        });
    };
    std::map<uint64_t, std::vector<Session::ClipCell>> decks;
    for (const Session::ClipCell& cell : cells)
        decks[columnDeck(composition, cell.column)].push_back(cell);
    std::vector<Edit> edits;
    auto clear = [&](const std::vector<Session::ClipCell>& group, size_t from) {
        for (size_t i = from; i < group.size(); ++i)
            for (const Effect& effect : session_.clip(group[i])->effects)
                edits.push_back(RemoveEffect{effect.id});
    };
    if (!cells.empty() && alike(cells) && (composition.decks.size() == 1 || decks.size() > 1)) {
        const std::vector<Effect>& chain = session_.clip(cells.front())->effects;
        for (size_t i = 0; i < chain.size(); ++i)
            edits.push_back(MoveEffect{chain[i].id, i, id});
        clear(cells, 1);
    } else {
        for (const auto& [deck, group] : decks) {
            if (!alike(group))
                continue;
            auto own = layer->decks.find(deck);
            DeckLayer settings = own == layer->decks.end() ? DeckLayer{} : own->second;
            std::vector<Effect> chain = session_.clip(group.front())->effects;
            for (Effect& effect : chain)
                effect.id = 0;
            for (Effect effect : settings.effects ? *settings.effects : layer->effects) {
                if (!settings.effects)
                    effect.id = 0;
                chain.push_back(effect);
            }
            settings.effects = chain;
            edits.push_back(SetDeckLayer{id, deck, settings});
            clear(group, 0);
        }
    }
    if (edits.empty())
        return session_.showStatus("The clips carry no effects alike to move", true);
    session_.edit(std::move(edits));
}

void LayerStack::moveLayerOnto(uint64_t layer, uint64_t onto)
{
    const Composition& composition = session_.composition();
    if (layer == onto || !session_.layer(layer) || !session_.layer(onto))
        return;
    const Layer* moving = session_.layer(layer);
    uint64_t parent = placeOf(composition, onto).first;
    if (moving->group)
        parent = 0;
    std::vector<uint64_t> order;
    for (const Layer& l : siblings(composition, parent))
        if (l.id != layer)
            order.push_back(l.id);
    uint64_t anchor = onto;
    if (moving->group && placeOf(composition, onto).first != 0)
        anchor = placeOf(composition, onto).first;
    const size_t at = size_t(std::find(order.begin(), order.end(), anchor) - order.begin());
    session_.client().edit(MoveLayer{layer, parent, std::min(at + 1, order.size())});
}

void LayerStack::setOpacity(const Row& row, int x)
{
    const auto bar = controlBounds(row).reduced(5, 0).withTrimmedRight(36);
    const double value = juce::jlimit(0.0, 1.0, double(x - bar.getX()) / bar.getWidth());
    dragOpacity_ = std::make_pair(row.layer, value);
    if (seconds() - lastOpacitySend_ >= kOpacitySendInterval) {
        session_.client().edit(SetLayerOpacity{row.layer, value});
        lastOpacitySend_ = seconds();
    }
    repaint();
}

void LayerStack::showBlendMenu(const Row& row)
{
    const Layer* layer = session_.layer(row.layer);
    if (!layer)
        return;
    juce::PopupMenu menu;
    const BlendMode blend = deckBlend(*layer, session_.composition().deck);
    for (int i = 0; i < kBlendModeCount; ++i)
        menu.addItem(i + 1, blendModeName(BlendMode(i)), true, blend == BlendMode(i));
    const uint64_t id = row.layer;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this, id](int result) {
        if (result > 0)
            session_.client().edit(blendEdit(session_.composition(), id, BlendMode(result - 1)));
    });
}

void LayerStack::showAutopilotMenu(const Row& row)
{
    const Layer* layer = session_.layer(row.layer);
    if (!layer)
        return;
    juce::PopupMenu menu;
    const Autopilot autopilot = deckAutopilot(*layer, session_.composition().deck);
    menu.addItem(1, "Autopilot off", true, autopilot.mode == AutopilotMode::Off);
    menu.addItem(2, "Next clip", true, autopilot.mode == AutopilotMode::Next);
    menu.addItem(3, "Random clip", true, autopilot.mode == AutopilotMode::Random);
    menu.addSeparator();
    for (int bars : {1, 2, 4, 8, 16})
        menu.addItem(10 + bars, juce::String(bars) + (bars == 1 ? " bar" : " bars"), true, autopilot.bars == bars);
    const uint64_t id = row.layer;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this, id](int result) {
        const Composition& c = session_.composition();
        if (result >= 1 && result <= 3)
            session_.client().edit(autopilotEdit(c, id, AutopilotMode(result - 1), std::nullopt));
        else if (result > 10)
            session_.client().edit(autopilotEdit(c, id, std::nullopt, result - 10));
    });
}

// Into an existing column's cell, or a new column at the end of the shown deck.
void LayerStack::place(uint64_t layer, int slot, const ClipRef& clip)
{
    if (auto column = columnAt(slot)) {
        session_.client().edit(SetSlot{layer, *column, clip});
        return;
    }
    if (const Deck* deck = session_.shownDeck())
        session_.client().edit(AddColumn{deck->id, std::nullopt, Column{}, {Cell{layer, 0, clip}}});
}

bool LayerStack::isInterestedInDragSource(const SourceDetails& details)
{
    const juce::String description = details.description.toString();
    return description.startsWith("media:") || description.startsWith("cell:") || isEffectDrag(description) ||
           description.startsWith("fxcard:") || description.startsWith("layer:") || description.startsWith("source:");
}

// An effect or an effect card lands on a layer's control (slot -1) or on a cell holding a clip; media on any cell or a
// layer's control; clips on any cell; a layer on another layer's control.
void LayerStack::itemDragMove(const SourceDetails& details)
{
    const Hit hit = partAt(details.localPosition);
    const Row* row = findRow(hit.layer);
    const juce::String description = details.description.toString();
    std::optional<std::pair<uint64_t, int>> target;
    if (isEffectDrag(description) || description.startsWith("fxcard:")) {
        if (row && (hit.slot < 0 || clipAt(*row, hit.slot)))
            target = std::make_pair(hit.layer, hit.slot);
    } else if (description.startsWith("layer:")) {
        if (row && hit.slot < 0 && hit.layer != uint64_t(description.fromFirstOccurrenceOf(":", false, false).getLargeIntValue()))
            target = std::make_pair(hit.layer, -1);
    } else if ((description.startsWith("media:") || description.startsWith("source:")) && row && !row->group &&
               hit.slot < 0 && hit.part != Part::None) {
        target = std::make_pair(hit.layer, -1);
    } else if (row && !row->group && hit.slot >= 0) {
        target = std::make_pair(hit.layer, hit.slot);
    }
    if (target != dropTarget_) {
        dropTarget_ = target;
        repaint();
    }
}

void LayerStack::itemDragExit(const SourceDetails&)
{
    dropTarget_.reset();
    repaint();
}

void LayerStack::itemDropped(const SourceDetails& details)
{
    const auto target = dropTarget_;
    dropTarget_.reset();
    repaint();
    if (!target)
        return;
    const juce::String description = details.description.toString();
    const juce::String rest = description.fromFirstOccurrenceOf(":", false, false);
    const ChainOwner owner{target->first, target->second >= 0 ? columnAt(target->second) : std::nullopt};
    if ((isEffectDrag(description) || description.startsWith("fxcard:")) && owner.column &&
        dropOnSelection(session_, description, {owner.layer, *owner.column}, std::nullopt))
        return;
    if (const auto effects = draggedEffects(session_, description); !effects.empty()) {
        session_.edit(addEffectsTo(session_.composition(), owner, std::nullopt, effects));
        if (owner.column && effects.size() == 1)
            session_.rememberClipEdit(Session::addingEffect(effects.front()));
        return;
    }
    if (description.startsWith("fxcard:")) {
        dropEffectCard(session_, description, owner, std::nullopt);
        return;
    }
    if (description.startsWith("layer:")) {
        moveLayerOnto(uint64_t(description.fromFirstOccurrenceOf(":", false, false).getLargeIntValue()), target->first);
        return;
    }
    if (description.startsWith("media:")) {
        session_.placeClips(target->first, target->second, clipsOf(description));
        return;
    }
    if (description.startsWith("source:")) {
        session_.placeClips(target->first, target->second, {generatorClip(rest.toStdString())});
        return;
    }
    const juce::StringArray parts = juce::StringArray::fromTokens(description, ":", "");
    const uint64_t fromLayer = uint64_t(parts[1].getLargeIntValue());
    const int fromSlot = parts[2].getIntValue();
    const Row* from = findRow(fromLayer);
    const ClipRef* clip = from ? clipAt(*from, fromSlot) : nullptr;
    if (!clip || (fromLayer == target->first && fromSlot == target->second))
        return;
    // A move keeps the clip's effect ids, so macros and modulators on them follow it; a copy (Alt) gets fresh ones.
    ClipRef moved = *clip;
    const uint64_t fromColumn = *columnAt(fromSlot);
    const bool copy = juce::ModifierKeys::currentModifiers.isAltDown();
    if (session_.isSelected({fromLayer, fromColumn}) && session_.selectedClips().size() > 1 && columnAt(target->second))
        return moveBlock({fromLayer, fromColumn}, {target->first, *columnAt(target->second)}, copy);
    session_.client().beginGesture();
    if (copy)
        for (Effect& effect : moved.effects)
            effect.id = 0;
    else
        session_.client().edit(SetSlot{fromLayer, fromColumn, std::nullopt});
    place(target->first, target->second, moved);
    session_.client().endGesture();
}

void LayerStack::fileDragMove(const juce::StringArray&, int x, int y)
{
    SourceDetails details({}, nullptr, {x, y});
    itemDragMove(details);
}

void LayerStack::fileDragExit(const juce::StringArray&)
{
    dropTarget_.reset();
    repaint();
}

// Files fill the layer's cells from the one they were dropped on; past the deck's last column they add columns.
void LayerStack::filesDropped(const juce::StringArray& dropped, int, int)
{
    const auto target = dropTarget_;
    dropTarget_.reset();
    repaint();
    if (!target)
        return;
    if (!session_.client().local()) {
        session_.showStatus("Files from this machine reach a remote engine with Node Sync; drop from its library", true);
        return;
    }
    const std::vector<std::string> files = mediaFiles(dropped);
    std::map<std::string, std::optional<uint64_t>> columns;
    const uint64_t deck = session_.shownDeck() ? session_.shownDeck()->id : 0;
    for (size_t i = 0; i < files.size(); ++i)
        columns[juce::File(files[i]).getFullPathName().toStdString()] = columnAt(target->second + int(i));
    EngineClient& client = session_.client();
    const uint64_t layer = target->first;
    client.addMedia(files, [&client, columns, layer, deck](const LibraryItem& item) {
        if (item.status != "ok")
            return;
        const ClipRef clip{item.hash, item.name};
        auto it = columns.find(item.path);
        if (it != columns.end() && it->second)
            client.edit(SetSlot{layer, *it->second, clip});
        else
            client.edit(AddColumn{deck, std::nullopt, Column{}, {Cell{layer, 0, clip}}});
    });
}

ColumnRow::ColumnRow(Session& session, LayerStack& stack) : session_(session), stack_(stack)
{
    session_.addView(this);
}

ColumnRow::~ColumnRow()
{
    session_.removeView(this);
}

void ColumnRow::paint(juce::Graphics& g)
{
    const Palette& p = palette();
    const Metrics& m = metrics();
    g.fillAll(p.background);
    g.setFont(Look::font());
    const bool compSelected = !session_.selectedLayer();
    const juce::Rectangle<int> composition{m.gap, 0, m.layerControlWidth, getHeight()};
    fillWithText(g, composition, compSelected ? juce::Colour(0xff3a3a3a) : juce::Colour(0xff1e1e1e),
                 compSelected ? p.white : p.muted, "Composition");
    if (dropOnComposition_) {
        g.setColour(p.white);
        g.drawRect(composition, 2);
    }
    const Deck* deck = session_.shownDeck();
    const uint64_t stepped = session_.composition().columnAutopilot.mode != AutopilotMode::Off
                                 ? session_.client().clock().columns.column
                                 : 0;
    const uint64_t active = stepped ? stepped : session_.composition().column;
    // The column headers scroll with the cells, under the pinned Composition header.
    g.saveState();
    g.reduceClipRegion(composition.getRight(), 0, getWidth() - composition.getRight(), getHeight());
    for (int slot = 0; slot < stack_.slots(); ++slot) {
        const auto area = headerArea(slot);
        if (!area.intersects(getLocalBounds()))
            continue;
        if (!deck || slot >= int(deck->columns.size())) {
            fillWithText(g, area, juce::Colour(0xff1e1e1e), juce::Colour(0xff555555), juce::String(slot + 1),
                         juce::Justification::centred);
            continue;
        }
        const Column& column = deck->columns[size_t(slot)];
        const bool live = column.id == active;
        fillWithText(g, area, live ? p.live : juce::Colour(0xff1e1e1e), live ? p.liveInk : p.text,
                     column.name.empty() ? juce::String(slot + 1) : utf8(column.name),
                     juce::Justification::centred, 2);
    }
    g.restoreState();
}

juce::Rectangle<int> ColumnRow::headerArea(int slot) const
{
    return stack_.columnArea(slot).withHeight(getHeight()).translated(-stack_.scrollX(), 0);
}

int ColumnRow::slotAt(juce::Point<int> point) const
{
    if (point.x < metrics().gap + metrics().layerControlWidth)
        return -1;
    const Deck* deck = session_.shownDeck();
    for (int slot = 0; deck && slot < int(deck->columns.size()); ++slot)
        if (headerArea(slot).contains(point))
            return slot;
    return -1;
}

bool ColumnRow::isInterestedInDragSource(const SourceDetails& details)
{
    const juce::String description = details.description.toString();
    return isEffectDrag(description) || description.startsWith("fxcard:");
}

void ColumnRow::itemDragMove(const SourceDetails& details)
{
    const bool over = details.localPosition.x < metrics().gap + metrics().layerControlWidth;
    if (over != dropOnComposition_) {
        dropOnComposition_ = over;
        repaint();
    }
}

void ColumnRow::itemDragExit(const SourceDetails&)
{
    dropOnComposition_ = false;
    repaint();
}

void ColumnRow::itemDropped(const SourceDetails& details)
{
    const bool over = dropOnComposition_;
    dropOnComposition_ = false;
    repaint();
    const juce::String description = details.description.toString();
    if (!over)
        return;
    if (description.startsWith("fxcard:")) {
        dropEffectCard(session_, description, {}, std::nullopt);
        return;
    }
    session_.edit(addEffectsTo(session_.composition(), {}, std::nullopt, draggedEffects(session_, description)));
}

void ColumnRow::rename(int slot)
{
    const Deck* deck = session_.shownDeck();
    if (!deck || slot < 0 || slot >= int(deck->columns.size()))
        return;
    const Column& column = deck->columns[size_t(slot)];
    editInline(editor_, *this, headerArea(slot), utf8(column.name),
               [this, id = column.id](const juce::String& name) {
                   session_.client().edit(Rename{"column", id, name.toStdString()});
               });
}

Menu ColumnRow::columnMenu(int slot)
{
    const Deck* deck = session_.shownDeck();
    if (!deck || slot < 0 || slot >= int(deck->columns.size()))
        return {};
    const uint64_t id = deck->columns[size_t(slot)].id;
    auto clear = [this, id] {
        session_.client().beginGesture();
        forEachLayer(session_.composition().layers, [&](const Layer& layer) {
            if (layer.clips.contains(id))
                session_.client().edit(SetSlot{layer.id, id, std::nullopt});
        });
        session_.client().endGesture();
    };
    return {{.label = "Fire column",
             .run = [this, id] { session_.client().triggerColumn(id); },
             .tip = "Every layer plays its clip in this column (live)"},
            {.label = "Rename", .run = [this, slot] { rename(slot); }, .tip = "Name the column (or double-click it)"},
            {.label = "Select all clips in column",
             .run = [this, id] { session_.selectClips(session_.deckClips(std::nullopt, id)); },
             .tip = "Select every clip of this column"},
            {},
            {.label = "Clear column", .run = clear, .tip = "Empty every cell of this column"}};
}

void ColumnRow::mouseDoubleClick(const juce::MouseEvent& e)
{
    rename(slotAt(e.getPosition()));
}

juce::String ColumnRow::tipAt(juce::Point<int> point)
{
    if (point.x < metrics().gap + metrics().layerControlWidth)
        return "The composition: click to edit its effects; drop an effect here to add it";
    if (slotAt(point) >= 0)
        return "Trigger the column: every layer's clip in it; double-click renames, right-click for more";
    return {};
}

void ColumnRow::mouseUp(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) {
        if (const int slot = slotAt(e.getPosition()); slot >= 0)
            showMenu(*this, columnMenu(slot));
        return;
    }
    if (e.x < metrics().gap + metrics().layerControlWidth) {
        session_.selectComposition();
        return;
    }
    if (const int slot = slotAt(e.getPosition()); slot >= 0)
        session_.client().triggerColumn(session_.shownDeck()->columns[size_t(slot)].id);
}

ClipGrid::ClipGrid(Session& session) : decks_(session), stack_(session), columns_(session, stack_)
{
    addAndMakeVisible(decks_);
    addAndMakeVisible(columns_);
    viewport_.setViewedComponent(&stack_, false);
    viewport_.setScrollBarsShown(true, true);
    viewport_.onScrolled = [this](int x) {
        if (!showing_ && x != stack_.scrollX())
            lastUserScroll_ = juce::Time::getMillisecondCounter();
        stack_.setScrollX(x);
        columns_.repaint();
    };
    stack_.onActiveSlot = [this](int slot) {
        if (!userScrolling())
            showSlot(slot);
    };
    addAndMakeVisible(viewport_);
}

bool ClipGrid::userScrolling() const
{
    return juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown() ||
           juce::Time::getMillisecondCounter() - lastUserScroll_ < kUserScrollHoldMs;
}

void ClipGrid::showSlot(int slot)
{
    if (slot < 0 || slot >= stack_.slots())
        return;
    const juce::ScopedValueSetter<bool> showing(showing_, true);
    const auto column = stack_.columnArea(slot);
    const int left = viewport_.getViewPositionX();
    const int width = viewport_.getViewWidth();
    const int pinned = metrics().gap * 2 + metrics().layerControlWidth;
    if (column.getX() - pinned < left)
        viewport_.setViewPosition(column.getX() - pinned, viewport_.getViewPositionY());
    else if (column.getRight() + metrics().gap > left + width)
        viewport_.setViewPosition(column.getRight() + metrics().gap - width, viewport_.getViewPositionY());
}

void ClipGrid::paint(juce::Graphics& g)
{
    g.fillAll(Look::palette().background);
}

void ClipGrid::resized()
{
    const Metrics& m = metrics();
    auto area = getLocalBounds();
    decks_.setBounds(area.removeFromTop(m.deckTabsHeight));
    area.removeFromTop(m.gap);
    columns_.setBounds(area.removeFromTop(m.columnRowHeight));
    area.removeFromTop(m.gap);
    viewport_.setBounds(area);
    const int width = area.getWidth() - viewport_.getScrollBarThickness();
    const int height = stack_.layout(width);
    stack_.setSize(stack_.contentWidth(), height);
    stack_.setScrollX(viewport_.getViewPositionX());
    columns_.repaint();
}

} // namespace azl::ui
