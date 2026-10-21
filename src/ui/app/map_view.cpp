#include "ui/app/map_view.hpp"

#include "document/mapping.hpp"
#include "ui/app/look.hpp"

namespace azl::ui {

namespace {

constexpr int kTreeWidth = 220;
constexpr int kInspectorWidth = 290;
constexpr int kToolbarHeight = 26;
constexpr int kRow = 22;
constexpr int kItemRow = 20;
constexpr int kEyeWidth = 22;
constexpr int kDragStart = 4;
enum PendingKind { kScreen, kSlice, kMask };

const char* const kToolbar[] = {"\xe2\x86\x90 All screens", "Output", "Input", "+ Slice", "+ Mask", "+ Guide |",
                                "+ Guide \xe2\x80\x94", "Test card: all screens", "Open large", "Guides", "Fit", "Lock"};

juce::String screenName(const Screen& screen)
{
    return screen.name.empty() ? "Screen " + juce::String(int64_t(screen.id)) : utf8(screen.name);
}

juce::String inputName(const Composition& composition, uint64_t input)
{
    if (input == 0)
        return "Composition";
    const Layer* found = nullptr;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (layer.id == input)
            found = &layer;
    });
    return found ? utf8(found->name) : "Layer " + juce::String(int64_t(input));
}

std::optional<size_t> screenIndex(const Composition& composition, uint64_t screen)
{
    const auto& screens = composition.screens;
    auto it = std::find_if(screens.begin(), screens.end(), [&](const Screen& s) { return s.id == screen; });
    if (it == screens.end())
        return std::nullopt;
    return size_t(it - screens.begin());
}

const Screen* screenWith(const Composition& composition, uint64_t screen)
{
    const std::optional<size_t> index = screenIndex(composition, screen);
    return index ? &composition.screens[*index] : nullptr;
}

} // namespace

MapView::MapView(Session& session) : session_(session), canvas_(session, *this), inspector_(session, *this)
{
    addAndMakeVisible(canvas_);
    addAndMakeVisible(inspector_);
    addAndMakeVisible(addScreen_);
    addScreen_.onClick = [this] { addScreen(); };
    for (const char* text : kToolbar) {
        auto button = std::make_unique<juce::TextButton>(juce::String::fromUTF8(text));
        addAndMakeVisible(*button);
        toolbar_.push_back(std::move(button));
    }
    auto on = [this](int i, std::function<void()> click) { toolbar_[size_t(i)]->onClick = std::move(click); };
    on(0, [this] { setFocus(0); });
    on(1, [this] { setInputView(false); });
    on(2, [this] { setInputView(true); });
    on(3, [this] { addSlice(); });
    on(4, [this] { addMask(); });
    on(5, [this] { addGuide(true); });
    on(6, [this] { addGuide(false); });
    on(7, [this] { toggleTestCard(0); });
    on(8, [this] { setFocus(state_.screen); });
    on(9, [this] {
        state_.guides = !state_.guides;
        changed();
    });
    on(10, [this] { canvas_.fit(); });
    on(11, [this] {
        state_.locked = !state_.locked;
        changed();
    });
    toolbar_[11]->setTooltip("Lock the mapping: select and look, but no handles, nudges or field edits");
    toolbar_[0]->setTooltip("Back to all screens (Esc)");
    toolbar_[1]->setTooltip("Where each slice lands on its output");
    toolbar_[2]->setTooltip("Which part of its input each slice takes");
    toolbar_[8]->setTooltip("Or double-click the screen's name");
    toolbar_[9]->setTooltip("Draw the guides through the slices on the outputs while Map is open");
    toolbar_[10]->setTooltip("Scroll on the map to zoom, drag an empty spot to pan");
    session_.addView(this);
}

MapView::~MapView()
{
    session_.removeView(this);
    if (!targetDisplay_.empty())
        session_.client().setTarget(targetDisplay_, std::nullopt);
    if (guidesShown_.value_or(false))
        session_.client().showGuides(false);
}

juce::Button* MapView::toolbarButton(const juce::String& text) const
{
    for (const auto& button : toolbar_)
        if (button->getButtonText() == text)
            return button.get();
    return text == addScreen_.getButtonText() ? const_cast<juce::TextButton*>(&addScreen_) : nullptr;
}

const Screen* MapView::screen() const
{
    for (const Screen& s : session_.composition().screens)
        if (s.id == state_.screen)
            return &s;
    return nullptr;
}

const Slice* MapView::slice() const
{
    if (const Screen* s = screen(); s && !state_.mask)
        for (const Slice& slice : s->slices)
            if (slice.id == state_.item)
                return &slice;
    return nullptr;
}

const Slice* MapView::maskOwner() const
{
    if (const Screen* s = screen(); s && state_.mask && state_.maskSlice)
        for (const Slice& slice : s->slices)
            if (slice.id == state_.maskSlice)
                return &slice;
    return nullptr;
}

const Mask* MapView::mask() const
{
    const Screen* s = screen();
    if (!s || !state_.mask)
        return nullptr;
    const Slice* owner = maskOwner();
    if (state_.maskSlice && !owner)
        return nullptr;
    for (const Mask& mask : owner ? owner->masks : s->masks)
        if (mask.id == state_.item)
            return &mask;
    return nullptr;
}

Points MapView::shape() const
{
    if (const Slice* s = slice()) {
        if (isCornerWarp(s->warp))
            return cornerPointsOf(*s);
        return warpAnchors(*s)[0] ? anchorsOf(*s) : cornersOf(*s);
    }
    const Mask* m = mask();
    if (!m)
        return {};
    const Slice* owner = maskOwner();
    if (!owner)
        return m->points;
    Points out;
    for (const Point& p : m->points)
        out.push_back(sliceToScreen(*owner, p));
    return out;
}

bool MapView::hasEdges() const
{
    const Slice* s = slice();
    return !s || (!warpAnchors(*s)[0] && !isCornerWarp(s->warp));
}

bool MapView::cornerWarp() const
{
    const Slice* s = slice();
    return s && isCornerWarp(s->warp);
}

Points MapView::movedPoint(const Points& shape, int index, Point delta) const
{
    if (cornerWarp() && shape.size() == 12)
        return movedCornerPoint(shape, index, delta);
    Points out = shape;
    out[size_t(index)][0] += delta[0];
    out[size_t(index)][1] += delta[1];
    return out;
}

Points MapView::frameOf(const Points& shape) const
{
    if (const Slice* s = slice()) {
        const auto [nx, ny] = warpAnchors(*s);
        if (nx && shape.size() == size_t(nx * ny))
            return outerCorners(shape, nx, ny);
        if (isCornerWarp(s->warp) && shape.size() == 12)
            return Points(shape.begin(), shape.begin() + 4);
    }
    return shape;
}

std::pair<double, double> MapView::pixelSize(const Screen& screen) const
{
    for (const OutputDisplay& d : session_.client().displays())
        if (d.id == screen.display && d.width > 0 && d.height > 0)
            return {double(d.width), double(d.height)};
    return {1920.0, 1080.0};
}

uint64_t MapView::input() const
{
    const Slice* s = slice();
    return s ? s->input : 0;
}

void MapView::validate()
{
    const auto& screens = session_.composition().screens;
    if (!screen())
        state_.screen = screens.empty() ? 0 : screens.front().id;
    const Screen* s = screen();
    if (state_.focus && std::none_of(screens.begin(), screens.end(), [&](const Screen& x) { return x.id == state_.focus; }))
        state_.focus = 0;
    if (!s) {
        state_.item = 0;
        return;
    }
    if (!slice() && !mask()) {
        state_.mask = false;
        state_.maskSlice = 0;
        state_.item = s->slices.empty() ? 0 : s->slices.front().id;
        state_.point = -1;
    }
    if (state_.point >= int(shape().size()))
        state_.point = -1;
}

void MapView::select(uint64_t screen, uint64_t item, bool mask)
{
    state_.screen = screen;
    state_.item = item;
    state_.mask = mask;
    state_.maskSlice = 0;
    if (mask)
        for (const Screen& s : session_.composition().screens)
            if (s.id == screen)
                for (const Slice& slice : s.slices)
                    for (const Mask& m : slice.masks)
                        if (m.id == item)
                            state_.maskSlice = slice.id;
    state_.point = -1;
    validate();
    session_.setMapScreen(state_.screen);
    changed();
}

void MapView::setFocus(uint64_t screen)
{
    state_.focus = screen;
    if (screen && screen != state_.screen)
        select(screen, 0, false);
    canvas_.fit();
    changed();
}

void MapView::setInputView(bool input)
{
    state_.input = input;
    canvas_.fit();
    changed();
}

bool MapView::escape()
{
    if (!state_.focus)
        return false;
    setFocus(0);
    return true;
}

void MapView::changed()
{
    validate();
    const Composition& composition = session_.composition();
    toolbar_[0]->setVisible(state_.focus && !state_.input);
    toolbar_[1]->setToggleState(!state_.input, juce::dontSendNotification);
    toolbar_[2]->setToggleState(state_.input, juce::dontSendNotification);
    toolbar_[7]->setToggleState(composition.testCard, juce::dontSendNotification);
    toolbar_[8]->setVisible(!state_.focus && !state_.input && screen());
    toolbar_[9]->setToggleState(state_.guides, juce::dontSendNotification);
    toolbar_[11]->setToggleState(state_.locked, juce::dontSendNotification);
    for (int i : {3, 4})
        toolbar_[size_t(i)]->setEnabled(screen() != nullptr && !state_.locked);
    layoutToolbar();
    if (isVisible()) {
        std::set<std::string> sources;
        for (const Screen& s : composition.screens)
            if (!s.hidden || s.id == state_.focus)
                sources.insert("screen:" + std::to_string(s.id));
        sources.insert(input() ? "layer:" + std::to_string(input()) : "composition");
        session_.setMapPreviews(sources);
    }
    updateOverlays();
    inspector_.refresh();
    canvas_.repaint();
    repaint();
}

void MapView::setLiveTarget(std::optional<Point> point)
{
    liveTarget_ = point;
    updateOverlays();
}

std::optional<Point> MapView::targetPoint() const
{
    const Points points = shape();
    if (state_.point >= 0 && state_.point < int(points.size()))
        return points[size_t(state_.point)];
    return std::nullopt;
}

void MapView::updateOverlays()
{
    EngineClient& client = session_.client();
    const bool guides = isVisible() && state_.guides;
    if (guidesShown_ != guides) {
        client.showGuides(guides);
        guidesShown_ = guides;
    }
    const Screen* s = screen();
    std::string display = s ? s->display : std::string();
    std::optional<Point> point;
    if (isVisible() && state_.targets && !state_.input && !display.empty())
        point = liveTarget_ ? liveTarget_ : targetPoint();
    if (!point)
        display.clear();
    if (display == targetDisplay_ && point == targetSent_)
        return;
    if (!targetDisplay_.empty() && display != targetDisplay_)
        client.setTarget(targetDisplay_, std::nullopt);
    if (!display.empty())
        client.setTarget(display, point);
    targetDisplay_ = display;
    targetSent_ = point;
}

void MapView::visibilityChanged()
{
    if (isVisible()) {
        validate();
        session_.setMapScreen(state_.screen);
        session_.client().requestOutputs();
    } else {
        session_.setMapScreen(0);
        session_.setMapPreviews({});
        liveTarget_.reset();
    }
    changed();
}

void MapView::sessionChanged(int changes)
{
    if (changes & Session::Document) {
        const Composition& composition = session_.composition();
        if (pending_) {
            const Screen* s = nullptr;
            for (const Screen& x : composition.screens)
                if (x.id == pending_->screen)
                    s = &x;
            if (pending_->kind == kScreen && composition.screens.size() > pending_->count) {
                select(composition.screens.back().id, 0, false);
                pending_.reset();
            } else if (pending_->kind == kSlice && s && s->slices.size() > pending_->count) {
                select(s->id, s->slices.back().id, false);
                pending_.reset();
            } else if (pending_->kind == kMask && s && !pending_->slice && s->masks.size() > pending_->count) {
                select(s->id, s->masks.back().id, true);
                pending_.reset();
            } else if (pending_->kind == kMask && s && pending_->slice) {
                for (const Slice& sl : s->slices)
                    if (sl.id == pending_->slice && sl.masks.size() > pending_->count) {
                        select(s->id, sl.masks.back().id, true);
                        pending_.reset();
                        break;
                    }
            }
        }
        if (isVisible() && !session_.mapScreen())
            session_.setMapScreen(state_.screen);
        canvas_.documentChanged();
    }
    if (changes & (Session::Document | Session::Outputs | Session::Connection)) {
        if (changes & Session::Connection) {
            guidesShown_.reset();
            targetDisplay_.clear();
            targetSent_.reset();
        }
        changed();
    } else if (changes & Session::Previews) {
        canvas_.repaint();
    }
}

void MapView::sendShape(const Points& points)
{
    EngineClient& client = session_.client();
    if (const Mask* m = mask()) {
        const Slice* owner = maskOwner();
        if (!owner)
            return client.edit(SetMask{m->id, clamped(points), std::nullopt, std::nullopt});
        if (points.size() != m->points.size())
            return;
        Points input;
        for (size_t i = 0; i < points.size(); ++i) {
            const auto p = screenToSlice(*owner, points[i], m->points[i]);
            input.push_back(p ? *p : m->points[i]);
        }
        return client.edit(SetMask{m->id, clamped(input), std::nullopt, std::nullopt});
    }
    const Slice* s = slice();
    if (!s)
        return;
    const Slice shaped = withShape(*s, points);
    const bool own = !client.gesturing();
    if (own)
        client.beginGesture();
    if (shaped.corners != s->corners)
        client.edit(SetSliceCorners{s->id, shaped.corners});
    if (!(shaped.warp == s->warp))
        client.edit(SetSliceWarp{s->id, true, shaped.warp.patches, shaped.warp.points});
    if (own)
        client.endGesture();
}

// The four corners of a corners warp; the handles are left out.
static Points outerCornerPoints(const Slice& slice)
{
    Points points = cornerPointsOf(slice);
    points.resize(std::min<size_t>(points.size(), 4));
    return points;
}

void MapView::setWarp(int n)
{
    const Slice* s = slice();
    if (!s)
        return;
    EngineClient& client = session_.client();
    const bool own = !client.gesturing();
    if (own)
        client.beginGesture();
    if (n <= 0) {
        if (const auto [nx, ny] = warpAnchors(*s); nx)
            client.edit(SetSliceCorners{s->id, cornerArray(clamped(outerCorners(anchorsOf(*s), nx, ny)))});
        else if (isCornerWarp(s->warp))
            client.edit(SetSliceCorners{s->id, cornerArray(clamped(outerCornerPoints(*s)))});
        client.edit(SetSliceWarp{s->id, false, std::array<int, 2>{1, 1}, std::vector<Point>{}});
    } else if (n == 1) {
        const Warp warp = azl::ui::cornerWarp(clamped(sampledCornerPoints(*s)));
        client.edit(SetSliceCorners{s->id, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0}});
        client.edit(SetSliceWarp{s->id, true, warp.patches, warp.points});
    } else {
        const Warp warp = interpolatingWarp(n, n, clamped(sampledAnchors(*s, n, n)));
        client.edit(SetSliceCorners{s->id, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0}});
        client.edit(SetSliceWarp{s->id, true, warp.patches, warp.points});
    }
    if (own)
        client.endGesture();
    state_.point = -1;
}

void MapView::flattenWarp()
{
    const Slice* s = slice();
    if (!s)
        return;
    Slice flat = *s;
    flat.warp = Warp{};
    if (isCornerWarp(s->warp)) {
        flat.corners = cornerArray(outerCornerPoints(*s));
        return sendShape(sampledCornerPoints(flat));
    }
    const auto [nx, ny] = warpAnchors(*s);
    if (!nx)
        return;
    flat.corners = cornerArray(outerCorners(anchorsOf(*s), nx, ny));
    sendShape(sampledAnchors(flat, nx, ny));
}

bool MapView::nudge(int dx, int dy)
{
    const Screen* s = screen();
    Points points = shape();
    if (!s || points.empty() || state_.locked)
        return false;
    const auto [w, h] = pixelSize(*s);
    const Point d{dx / w, dy / h};
    sendShape(state_.point >= 0 ? movedPoint(points, state_.point, d) : transformed(points, d[0], d[1]));
    return true;
}

void MapView::renameScreen(uint64_t screen, const std::string& name)
{
    session_.client().edit(Rename{"screen", screen, name});
}

void MapView::sendInputRect(const std::array<double, 4>& rect)
{
    if (const Slice* s = slice())
        session_.client().edit(SetSliceInput{s->id, std::nullopt, rect});
}

void MapView::addScreen()
{
    const Composition& composition = session_.composition();
    Screen added{.name = "Screen " + std::to_string(composition.screens.size() + 1), .slices = {Slice{}}};
    for (const OutputDisplay& d : session_.client().displays())
        if (added.display.empty() && std::none_of(composition.screens.begin(), composition.screens.end(),
                                                  [&](const Screen& s) { return s.display == d.id; }))
            added.display = d.id;
    pending_ = Pending{kScreen, 0, 0, composition.screens.size()};
    session_.client().edit(AddScreen{std::nullopt, added});
}

void MapView::addSlice(std::optional<std::array<double, 4>> inputRect)
{
    const Screen* s = screen();
    if (!s)
        return;
    Slice added{.input = input()};
    if (inputRect)
        added.inputRect = *inputRect;
    else
        added.corners = {0.2, 0.2, 0.8, 0.2, 0.8, 0.8, 0.2, 0.8};
    pending_ = Pending{kSlice, s->id, 0, s->slices.size()};
    session_.client().edit(AddSlice{s->id, std::nullopt, added});
}

void MapView::addMask(bool onSlice)
{
    const Screen* s = screen();
    if (!s)
        return;
    const Slice* owner = onSlice ? slice() : nullptr;
    if (onSlice && !owner)
        return;
    Mask added{.points = {{0.35, 0.3}, {0.65, 0.3}, {0.65, 0.7}, {0.35, 0.7}}};
    if (owner) {
        const auto& r = owner->inputRect;
        for (Point& p : added.points)
            p = {r[0] + p[0] * r[2], r[1] + p[1] * r[3]};
    }
    pending_ = Pending{kMask, s->id, owner ? owner->id : 0, owner ? owner->masks.size() : s->masks.size()};
    session_.client().edit(AddMask{s->id, owner ? owner->id : 0, std::nullopt, added});
}

void MapView::addGuide(bool vertical)
{
    std::vector<Guide> guides = session_.composition().guides;
    if (guides.size() >= kMaxGuides)
        return;
    guides.push_back({vertical, 0.5});
    session_.client().edit(SetGuides{guides});
}

void MapView::toggleTestCard(uint64_t id)
{
    bool on = session_.composition().testCard;
    for (const Screen& s : session_.composition().screens)
        if (s.id == id)
            on = s.testCard;
    session_.client().edit(SetScreenTestCard{id, !on});
}

void MapView::setHidden(uint64_t screen, bool hidden)
{
    session_.client().edit(SetScreenHidden{screen, hidden});
}

void MapView::moveScreen(uint64_t screen, size_t index)
{
    session_.client().edit(MoveScreen{screen, index});
}

void MapView::autoSoftEdge()
{
    if (const Slice* s = slice())
        session_.client().edit(SetSoftEdges{{}, s->id});
}

void MapView::removeSelected()
{
    if (const Mask* m = mask())
        session_.client().edit(RemoveMask{m->id});
    else if (const Slice* s = slice())
        session_.client().edit(RemoveSlice{s->id});
}

std::vector<MapView::TreeRow> MapView::treeRows() const
{
    std::vector<TreeRow> rows;
    int y = kRow - treeScroll_;
    for (const Screen& s : session_.composition().screens) {
        rows.push_back({{0, y, kTreeWidth, kRow}, s.id, 0, false});
        y += kRow + 1;
        for (const Slice& slice : s.slices) {
            rows.push_back({{0, y, kTreeWidth, kItemRow}, s.id, slice.id, false});
            y += kItemRow;
            for (const Mask& mask : slice.masks) {
                rows.push_back({{0, y, kTreeWidth, kItemRow}, s.id, mask.id, true, slice.id});
                y += kItemRow;
            }
        }
        for (const Mask& mask : s.masks) {
            rows.push_back({{0, y, kTreeWidth, kItemRow}, s.id, mask.id, true});
            y += kItemRow;
        }
        y += 2;
    }
    return rows;
}

std::vector<juce::Rectangle<int>> MapView::treeRowAreas() const
{
    std::vector<juce::Rectangle<int>> areas;
    for (const TreeRow& row : treeRows())
        areas.push_back(row.area);
    return areas;
}

juce::Rectangle<int> MapView::treeArea() const
{
    return getLocalBounds().withWidth(kTreeWidth).withTrimmedTop(kRow).withTrimmedBottom(kRow + 4);
}

std::optional<MapView::TreeRow> MapView::treeRowAt(juce::Point<int> point) const
{
    if (treeArea().contains(point))
        for (const TreeRow& row : treeRows())
            if (row.area.contains(point))
                return row;
    return std::nullopt;
}

juce::Rectangle<int> MapView::eyeArea(juce::Rectangle<int> row)
{
    return row.removeFromRight(kEyeWidth);
}

std::optional<size_t> MapView::dropIndexAt(juce::Point<int> point) const
{
    std::optional<size_t> index;
    for (const TreeRow& row : treeRows())
        if (!row.item && row.area.getY() <= point.y)
            index = screenIndex(session_.composition(), row.screen);
    if (!index && !session_.composition().screens.empty())
        return size_t(0);
    return index;
}

void MapView::mouseDown(const juce::MouseEvent& e)
{
    listDrag_.reset();
    const auto row = treeRowAt(e.getPosition());
    if (row && !row->item && !eyeArea(row->area).contains(e.getPosition()) && !e.mods.isPopupMenu())
        listDrag_ = ListDrag{row->screen, e.getPosition()};
}

void MapView::mouseDrag(const juce::MouseEvent& e)
{
    if (!listDrag_ || (!listDrag_->target && e.getPosition().getDistanceFrom(listDrag_->start) < kDragStart))
        return;
    listDrag_->target = dropIndexAt(e.getPosition());
    repaint();
}

void MapView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto rows = treeRows();
    if (!treeArea().contains(e.getPosition()) || rows.empty())
        return;
    treeScroll_ = wheelScrolled(treeScroll_, wheel, rows.back().area.getBottom() + treeScroll_ - kRow, treeArea().getHeight());
    repaint();
}

void MapView::layoutToolbar()
{
    auto bar = getLocalBounds().withTrimmedLeft(kTreeWidth).withTrimmedRight(kInspectorWidth).removeFromTop(kToolbarHeight);
    bar.reduce(3, 3);
    for (const auto& button : toolbar_) {
        if (!button->isVisible())
            continue;
        const int width = juce::GlyphArrangement::getStringWidthInt(Look::font(), button->getButtonText()) + 14;
        button->setBounds(bar.removeFromLeft(width));
        bar.removeFromLeft(3);
    }
}

void MapView::resized()
{
    auto area = getLocalBounds();
    auto tree = area.removeFromLeft(kTreeWidth);
    addScreen_.setBounds(tree.removeFromBottom(kRow + 4).reduced(4, 2));
    inspector_.setBounds(area.removeFromRight(kInspectorWidth).withTrimmedLeft(1));
    area.removeFromTop(kToolbarHeight);
    canvas_.setBounds(area);
    layoutToolbar();
}

void MapView::mouseUp(const juce::MouseEvent& e)
{
    const std::optional<ListDrag> drag = std::exchange(listDrag_, std::nullopt);
    if (drag && drag->target) {
        repaint();
        if (drag->target != screenIndex(session_.composition(), drag->screen))
            moveScreen(drag->screen, *drag->target);
        return;
    }
    const auto row = treeRowAt(e.getPosition());
    if (!row)
        return;
    if (const Screen* s = screenWith(session_.composition(), row->screen);
        s && !row->item && eyeArea(row->area).contains(e.getPosition()))
        return setHidden(s->id, !s->hidden);
    select(row->screen, row->item, row->mask);
    if (!e.mods.isPopupMenu() || row->item)
        return;
    juce::PopupMenu menu;
    menu.addItem(1, "Rename");
    menu.addItem(2, state_.focus == row->screen ? "All screens" : "Open large");
    juce::Component::SafePointer<MapView> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                       [self, id = row->screen](int result) {
                           if (!self)
                               return;
                           if (result == 1)
                               self->startRename(id);
                           else if (result == 2)
                               self->setFocus(self->state_.focus == id ? 0 : id);
                       });
}

void MapView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (const auto row = treeRowAt(e.getPosition()); row && !row->item && !eyeArea(row->area).contains(e.getPosition()))
        setFocus(state_.focus == row->screen ? 0 : row->screen);
}

bool MapView::keyPressed(const juce::KeyPress& key)
{
    if (key != juce::KeyPress(juce::KeyPress::F2Key) && key != juce::KeyPress(juce::KeyPress::returnKey))
        return false;
    if (!screen() || renaming_)
        return false;
    startRename(state_.screen);
    return true;
}

void MapView::startRename(uint64_t id)
{
    const Screen* s = nullptr;
    for (const Screen& x : session_.composition().screens)
        if (x.id == id)
            s = &x;
    for (const TreeRow& row : treeRows())
        if (s && row.screen == id && !row.item) {
            editInline(renaming_, *this, row.area.withWidth(kTreeWidth - 70), screenName(*s),
                       [this, id](const juce::String& name) { renameScreen(id, name.toStdString()); });
            return;
        }
}

void MapView::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.line);
    const Composition& composition = session_.composition();
    g.setColour(p.panel);
    g.fillRect(getLocalBounds().removeFromLeft(kTreeWidth));
    g.setFont(Look::font(true));
    fillWithText(g, {0, 0, kTreeWidth, kRow}, p.header, p.muted, "Screens");
    g.setFont(Look::font());
    g.saveState();
    g.reduceClipRegion(treeArea());
    for (const TreeRow& row : treeRows()) {
        const Screen* s = nullptr;
        for (const Screen& x : composition.screens)
            if (x.id == row.screen)
                s = &x;
        if (!s)
            continue;
        if (!row.item) {
            const bool on = row.screen == state_.screen;
            const juce::Colour ink = s->hidden ? p.muted : on ? p.white : p.text;
            fillWithText(g, row.area, on ? p.liveRow : p.cell, ink, screenName(*s));
            g.setColour(p.muted);
            drawText(g, s->display.empty() ? juce::String("no output") : utf8(s->display),
                     row.area.reduced(6, 0).withTrimmedRight(kEyeWidth - 4), juce::Justification::centredRight, true);
            g.setColour(ink);
            drawText(g, juce::String::fromUTF8(s->hidden ? "\xe2\x97\x8b" : "\xe2\x97\x8f"), eyeArea(row.area),
                     juce::Justification::centred, false);
            if (listDrag_ && listDrag_->target == screenIndex(composition, s->id)) {
                g.setColour(p.live);
                g.drawRect(row.area, 2);
            }
            continue;
        }
        const bool on = row.screen == state_.screen && row.item == state_.item && row.mask == state_.mask;
        juce::String name, note;
        int indent = 18;
        if (row.mask) {
            const std::vector<Mask>* masks = &s->masks;
            for (const Slice& sl : s->slices)
                if (sl.id == row.owner)
                    masks = &sl.masks;
            if (row.owner)
                indent = 32;
            for (const Mask& m : *masks)
                if (m.id == row.item)
                    name = juce::String::fromUTF8("\xe2\x97\xaa ") + utf8(m.name),
                    note = juce::String(m.invert ? "inverted" : "mask") + (row.owner ? " on slice" : "");
        } else {
            for (const Slice& sl : s->slices)
                if (sl.id == row.item)
                    name = juce::String::fromUTF8("\xe2\x96\xad ") + utf8(sl.name), note = inputName(composition, sl.input);
        }
        if (on) {
            g.setColour(p.button);
            g.fillRect(row.area);
        }
        g.setColour(on ? (row.mask ? p.accent : p.live) : p.text);
        drawText(g, name, row.area.withTrimmedLeft(indent).withTrimmedRight(6), juce::Justification::centredLeft, true);
        g.setColour(p.muted);
        g.setFont(Look::smallFont());
        drawText(g, note, row.area.reduced(6, 0), juce::Justification::centredRight, true);
        g.setFont(Look::font());
    }
    if (composition.screens.empty())
        fillWithText(g, {0, kRow, kTreeWidth, kRow}, p.cell, p.muted, "No screens");
    g.restoreState();

    auto bar = getLocalBounds().withTrimmedLeft(kTreeWidth).withTrimmedRight(kInspectorWidth).removeFromTop(kToolbarHeight);
    g.setColour(p.bar);
    g.fillRect(bar);
    int right = bar.getX();
    for (const auto& button : toolbar_)
        if (button->isVisible())
            right = std::max(right, button->getRight());
    g.setColour(p.muted);
    g.setFont(Look::smallFont());
    drawText(g, juce::String(canvas_.zoomPercent()) + "%   " +
                   (state_.input ? juce::String::fromUTF8("drag: move \xc2\xb7 Shift+drag: new slice \xc2\xb7 Alt: no snap")
                                 : juce::String::fromUTF8("Scroll: zoom \xc2\xb7 drag empty space: pan")),
               bar.withLeft(right + 8).reduced(0, 2), juce::Justification::centredLeft, true);
}

} // namespace azl::ui
