#include "ui/app/map_view.hpp"

#include "document/mapping.hpp"
#include "ui/app/look.hpp"
#include "ui/app/monitors.hpp"

#include <cmath>

namespace azl::ui {

namespace {

constexpr double kSendInterval = 1.0 / 30.0;
constexpr float kGrab = 8.0f;
constexpr float kRotateReach = 24.0f;
constexpr double kSnapPixels = 6.0;

double seconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

juce::Point<float> toJuce(Point p)
{
    return {float(p[0]), float(p[1])};
}

Point fromJuce(juce::Point<float> p)
{
    return {p.x, p.y};
}

juce::Path polygon(const std::vector<juce::Point<float>>& points)
{
    juce::Path path;
    for (size_t i = 0; i < points.size(); ++i)
        i == 0 ? path.startNewSubPath(points[i]) : path.lineTo(points[i]);
    path.closeSubPath();
    return path;
}

void square(juce::Graphics& g, juce::Point<float> at, float size, juce::Colour fill, juce::Colour edge)
{
    const auto r = juce::Rectangle<float>(size, size).withCentre(at);
    g.setColour(fill);
    g.fillRect(r);
    g.setColour(edge);
    g.drawRect(r, 1.5f);
}

std::string inputSource(uint64_t input)
{
    return input ? "layer:" + std::to_string(input) : "composition";
}

} // namespace

MapCanvas::MapCanvas(Session& session, MapView& map) : session_(session), map_(map)
{
    setWantsKeyboardFocus(false);
}

std::vector<const Screen*> MapCanvas::shownScreens() const
{
    const uint64_t focus = map_.state().focus;
    std::vector<const Screen*> shown;
    for (const Screen& s : session_.composition().screens)
        if (focus ? s.id == focus : !s.hidden)
            shown.push_back(&s);
    return shown;
}

BoardRect MapCanvas::frameOf(uint64_t screen) const
{
    if (map_.state().focus)
        return frameRect(0, true);
    if (liveFrame_ && liveFrame_->first == screen)
        return {liveFrame_->second[0], liveFrame_->second[1], kFrameW, kFrameH};
    return mapFrame(session_.composition().screens, screen);
}

BoardRect MapCanvas::board() const
{
    if (map_.state().input)
        return kInputBoard;
    if (map_.state().focus)
        return boardBounds(1, true);
    return mapBounds(session_.composition().screens);
}

bool MapCanvas::onTitle(uint64_t screen, juce::Point<float> pixel) const
{
    const Point u = unitAt(frameOf(screen), pixel);
    return u[1] < 0 && u[1] > -20.0 / (viewport().scale * kFrameH) && u[0] >= 0 && u[0] <= 1;
}

Viewport MapCanvas::viewport() const
{
    return user_ ? *user_ : fitted(board(), std::max(1, getWidth()), std::max(1, getHeight()));
}

void MapCanvas::fit()
{
    user_.reset();
    repaint();
    map_.repaint();
}

int MapCanvas::zoomPercent() const
{
    const Viewport fit = fitted(board(), std::max(1, getWidth()), std::max(1, getHeight()));
    return int(std::round(viewport().scale / fit.scale * 100.0));
}

Point MapCanvas::unitAt(const BoardRect& frame, juce::Point<float> pixel) const
{
    return frame.unit(viewport().toBoard(fromJuce(pixel)));
}

juce::Point<float> MapCanvas::pixelAt(const BoardRect& frame, Point unit) const
{
    return toJuce(viewport().toPixel(frame.at(unit)));
}

juce::Point<int> MapCanvas::screenPixel(uint64_t screen, Point unit) const
{
    return pixelAt(frameOf(screen), unit).roundToInt();
}

juce::Point<int> MapCanvas::inputPixel(Point unit) const
{
    return pixelAt(kInputRect, unit).roundToInt();
}

juce::Point<int> MapCanvas::titlePixel(uint64_t screen) const
{
    const BoardRect f = frameOf(screen);
    return (pixelAt(f, {0.0, 0.0}) + juce::Point<float>(30.0f, -9.0f)).roundToInt();
}

juce::Point<int> MapCanvas::rotatePixel() const
{
    const Screen* s = map_.screen();
    const Points pts = map_.frameOf(livePoints_ ? *livePoints_ : map_.shape());
    if (!s || pts.size() < 2)
        return {};
    const BoardRect f = frameOf(s->id);
    const auto c = pixelAt(f, centroid(pts));
    const auto m = (pixelAt(f, pts[0]) + pixelAt(f, pts[1])) / 2.0f;
    auto dir = m - c;
    const float len = dir.getDistanceFromOrigin();
    dir = len > 0.001f ? dir / len : juce::Point<float>(0.0f, -1.0f);
    return (m + dir * kRotateReach).roundToInt();
}

Points MapCanvas::outlineOf(const Screen& screen, uint64_t item, bool mask, uint64_t owner) const
{
    const MapState& state = map_.state();
    const bool selected = state.screen == screen.id && state.item == item && state.mask == mask;
    if (mask) {
        if (selected && livePoints_)
            return *livePoints_;
        for (const Slice& s : screen.slices)
            if (owner && s.id == owner)
                for (const Mask& m : s.masks)
                    if (m.id == item) {
                        // Each edge sampled, since it bends through the slice's warp and corner pin.
                        Points out;
                        for (size_t k = 0; k < m.points.size(); ++k)
                            for (int t = 0; t < 8; ++t) {
                                const Point& a = m.points[k];
                                const Point& b = m.points[(k + 1) % m.points.size()];
                                out.push_back(sliceToScreen(s, {a[0] + (b[0] - a[0]) * t / 8, a[1] + (b[1] - a[1]) * t / 8}));
                            }
                        return out;
                    }
        for (const Mask& m : screen.masks)
            if (!owner && m.id == item)
                return m.points;
        return {};
    }
    for (const Slice& s : screen.slices)
        if (s.id == item)
            return sliceOutline(selected && livePoints_ ? withShape(s, *livePoints_) : s);
    return {};
}

void MapCanvas::documentChanged()
{
    if (drag_)
        return;
    livePoints_.reset();
    liveRect_.reset();
    liveFrame_.reset();
    repaint();
}

void MapCanvas::paint(juce::Graphics& g)
{
    g.fillAll(Look::palette().mapCanvas);
    if (map_.state().input)
        paintInput(g);
    else
        paintOutput(g);
}

void MapCanvas::paintOutput(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const MapState& state = map_.state();
    for (const Screen* s : shownScreens()) {
        const BoardRect f = frameOf(s->id);
        const auto r = juce::Rectangle<float>(pixelAt(f, {0.0, 0.0}), pixelAt(f, {1.0, 1.0}));
        const bool on = s->id == state.screen;
        g.setFont(Look::font());
        g.setColour(on ? p.white : p.muted);
        juce::String title = utf8(s->name) + juce::String::fromUTF8(" \xc2\xb7 ") +
                             (s->display.empty() ? juce::String("no output") : utf8(s->display));
        if (s->testCard || session_.composition().testCard)
            title << juce::String::fromUTF8(" \xc2\xb7 test card");
        if (state.focus)
            title << "   (double-click or Esc: all screens)";
        drawText(g, title, juce::Rectangle<float>(r.getX(), r.getY() - 18.0f, std::max(r.getWidth(), 400.0f), 16.0f),
                   juce::Justification::centredLeft, true);
        g.setColour(juce::Colours::black);
        g.fillRect(r);
        const juce::Image picture = session_.preview("screen:" + std::to_string(s->id));
        if (!session_.previewsOn())
            drawPreviewsOff(g, r);
        else if (picture.isValid())
            g.drawImage(picture, r, juce::RectanglePlacement::stretchToFit);
        g.setColour(on ? p.live : p.mapFrame);
        g.drawRect(r, on ? 2.0f : 1.0f);

        for (const Slice& slice : s->slices) {
            const bool selected = on && !state.mask && slice.id == state.item;
            std::vector<juce::Point<float>> pts;
            for (const Point& q : outlineOf(*s, slice.id, false, 0))
                pts.push_back(pixelAt(f, q));
            g.setColour(selected ? p.live : on ? p.mapSliceOn : p.mapSlice);
            g.strokePath(polygon(pts), juce::PathStrokeType(selected ? 1.8f : 1.0f));
        }
        std::vector<std::pair<const Mask*, uint64_t>> masks;
        for (const Mask& mask : s->masks)
            masks.push_back({&mask, 0});
        for (const Slice& slice : s->slices)
            for (const Mask& mask : slice.masks)
                masks.push_back({&mask, slice.id});
        for (const auto& [maskPtr, owner] : masks) {
            const Mask& mask = *maskPtr;
            const bool selected = on && state.mask && mask.id == state.item;
            std::vector<juce::Point<float>> pts;
            for (const Point& q : outlineOf(*s, mask.id, true, owner))
                pts.push_back(pixelAt(f, q));
            const juce::Path path = polygon(pts);
            if (selected) {
                g.setColour(p.accent.withAlpha(0.12f));
                g.fillPath(path);
            }
            juce::Path dashed;
            const float dashes[] = {4.0f, 3.0f};
            juce::PathStrokeType(1.0f).createDashedStroke(dashed, path, dashes, 2);
            g.setColour(selected ? p.accent : on ? p.mapMaskOn : p.mapMask);
            g.fillPath(dashed);
        }
        if (on && state.item)
            paintHandles(g, *s, f);
    }
}

void MapCanvas::paintHandles(juce::Graphics& g, const Screen& screen, const BoardRect& f)
{
    const Palette& p = Look::palette();
    const MapState& state = map_.state();
    const juce::Colour colour = state.mask ? p.accent : p.live;
    const Points pts = livePoints_ ? *livePoints_ : map_.shape();
    if (pts.size() < 2 || state.locked)
        return;
    if (map_.cornerWarp() && pts.size() == 12) {
        g.setColour(colour.withAlpha(0.6f));
        for (int k = 0; k < 4; ++k)
            for (int h : cornerHandles(k))
                g.drawLine({pixelAt(f, pts[size_t(k)]), pixelAt(f, pts[size_t(h)])}, 1.0f);
    } else if (const Slice* slice = map_.slice(); slice && !map_.hasEdges()) {
        g.setColour(colour.withAlpha(0.7f));
        for (const Points& line : warpCurves(livePoints_ ? withShape(*slice, pts) : *slice)) {
            juce::Path path;
            for (size_t i = 0; i < line.size(); ++i)
                i == 0 ? path.startNewSubPath(pixelAt(f, line[i])) : path.lineTo(pixelAt(f, line[i]));
            g.strokePath(path, juce::PathStrokeType(1.0f));
        }
    } else {
        for (size_t k = 0; k < pts.size(); ++k) {
            const auto m = (pixelAt(f, pts[k]) + pixelAt(f, pts[(k + 1) % pts.size()])) / 2.0f;
            square(g, m, 8.0f, juce::Colour(0xff111111), colour);
        }
    }
    const Points frame = map_.frameOf(pts);
    const auto rot = rotatePixel().toFloat();
    const auto mid = (pixelAt(f, frame[0]) + pixelAt(f, frame[1])) / 2.0f;
    g.setColour(colour);
    g.drawLine({mid, rot}, 1.0f);
    g.setColour(juce::Colour(0xff111111));
    g.fillEllipse(juce::Rectangle<float>(12.0f, 12.0f).withCentre(rot));
    g.setColour(colour);
    g.drawEllipse(juce::Rectangle<float>(12.0f, 12.0f).withCentre(rot), 1.5f);
    const float size = map_.hasEdges() ? 11.0f : 9.0f;
    for (size_t k = 0; k < pts.size(); ++k) {
        const juce::Colour fill = int(k) == state.point ? p.white : colour;
        if (map_.cornerWarp() && k >= 4) {
            const auto r = juce::Rectangle<float>(9.0f, 9.0f).withCentre(pixelAt(f, pts[k]));
            g.setColour(int(k) == state.point ? p.white : juce::Colour(0xff111111));
            g.fillEllipse(r);
            g.setColour(colour);
            g.drawEllipse(r, 1.5f);
            continue;
        }
        square(g, pixelAt(f, pts[k]), map_.cornerWarp() ? 11.0f : size, fill, juce::Colours::black);
    }
}

void MapCanvas::paintInput(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const MapState& state = map_.state();
    const Composition& composition = session_.composition();
    const uint64_t input = map_.input();
    const auto area = juce::Rectangle<float>(pixelAt(kInputRect, {0.0, 0.0}), pixelAt(kInputRect, {1.0, 1.0}));
    g.setColour(juce::Colours::black);
    g.fillRect(area);
    const juce::Image picture = session_.preview(inputSource(input));
    if (!session_.previewsOn()) {
        drawPreviewsOff(g, area);
    } else if (picture.isValid()) {
        g.setOpacity(0.75f);
        g.drawImage(picture, area, juce::RectanglePlacement::stretchToFit);
        g.setOpacity(1.0f);
    }
    g.setColour(juce::Colour(0xff555555));
    g.drawRect(area, 1.0f);
    juce::String title = input ? "Layer " + juce::String(int64_t(input)) : juce::String("Composition");
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (layer.id == input)
            title = utf8(layer.name);
    });
    g.setFont(Look::font());
    g.setColour(p.muted);
    drawText(g, title + juce::String::fromUTF8(" \xc2\xb7 input"), area.withY(area.getY() - 18.0f).withHeight(16.0f),
               juce::Justification::centredLeft, true);

    struct Shown {
        const Screen* screen;
        const Slice* slice;
        int colour;
        std::array<double, 4> rect;
    };
    std::vector<Shown> shown;
    const Shown* selected = nullptr;
    for (size_t i = 0; i < composition.screens.size(); ++i)
        for (const Slice& slice : composition.screens[i].slices)
            if (slice.input == input) {
                const bool on = !state.mask && slice.id == state.item;
                shown.push_back({&composition.screens[i], &slice, int(i), on && liveRect_ ? *liveRect_ : slice.inputRect});
            }
    for (const Shown& s : shown)
        if (!state.mask && s.slice->id == state.item)
            selected = &s;
    auto rectPixels = [&](const std::array<double, 4>& r) {
        return juce::Rectangle<float>(pixelAt(kInputRect, {r[0], r[1]}), pixelAt(kInputRect, {r[0] + r[2], r[1] + r[3]}));
    };
    if (selected)
        for (const Shown& other : shown)
            if (&other != selected)
                if (const auto o = overlap(selected->rect, other.rect)) {
                    const auto r = rectPixels(*o);
                    g.saveState();
                    g.reduceClipRegion(r.toNearestInt());
                    g.setColour(p.accent.withAlpha(0.15f));
                    g.fillRect(r);
                    g.setColour(p.accent);
                    for (float x = r.getX() - r.getHeight(); x < r.getRight(); x += 8.0f)
                        g.drawLine(x, r.getBottom(), x + r.getHeight(), r.getY(), 2.0f);
                    g.restoreState();
                }
    std::vector<const Shown*> order;
    for (const Shown& s : shown)
        if (&s != selected)
            order.push_back(&s);
    if (selected)
        order.push_back(selected);
    g.setFont(Look::smallFont());
    for (const Shown* s : order) {
        const bool on = s == selected;
        const juce::Colour colour = p.screenColours[size_t(s->colour) % p.screenColours.size()];
        const auto r = rectPixels(s->rect);
        if (on) {
            g.setColour(p.live.withAlpha(0.08f));
            g.fillRect(r);
        }
        g.setColour(colour);
        g.drawRect(r, on ? 2.2f : 1.2f);
        drawText(g, utf8(s->screen->name) + juce::String::fromUTF8(" \xc2\xb7 ") + utf8(s->slice->name),
                   r.reduced(5.0f, 3.0f).withHeight(14.0f), juce::Justification::centredLeft, true);
        if (!on)
            continue;
        const juce::Point<float> handles[] = {r.getTopLeft(), r.getTopRight(), r.getBottomRight(), r.getBottomLeft(),
                                              {r.getCentreX(), r.getY()}, {r.getRight(), r.getCentreY()},
                                              {r.getCentreX(), r.getBottom()}, {r.getX(), r.getCentreY()}};
        for (int k = 0; k < 8; ++k)
            k < 4 ? square(g, handles[k], 11.0f, colour, juce::Colours::black)
                  : square(g, handles[k], 9.0f, juce::Colour(0xff111111), colour);
    }
    if (newRect_) {
        const auto r = rectPixels(*newRect_);
        g.setColour(p.live.withAlpha(0.1f));
        g.fillRect(r);
        juce::Path outline;
        outline.addRectangle(r);
        juce::Path dashed;
        const float dashes[] = {6.0f, 4.0f};
        juce::PathStrokeType(1.5f).createDashedStroke(dashed, outline, dashes, 2);
        g.setColour(p.live);
        g.fillPath(dashed);
    }
}

// The title of a frame drags the frame around the board; the view holds still meanwhile.
std::optional<MapCanvas::Drag> MapCanvas::grabTitle(const juce::MouseEvent& e)
{
    if (map_.state().focus)
        return std::nullopt;
    for (const Screen* s : shownScreens()) {
        if (!onTitle(s->id, e.position))
            continue;
        if (s->id != map_.state().screen)
            map_.select(s->id, 0, false);
        const BoardRect f = frameOf(s->id);
        Drag d;
        d.grab = Grab::Frame;
        d.screen = s->id;
        d.start = {f.x, f.y};
        user_ = viewport();
        return d;
    }
    return std::nullopt;
}

std::optional<MapCanvas::Drag> MapCanvas::grabOutput(const juce::MouseEvent& e)
{
    MapState& state = map_.state();
    const auto pos = e.position;
    auto start = [&](Grab grab, int index, const Screen& s) {
        Drag d;
        d.grab = grab;
        d.index = index;
        d.screen = s.id;
        d.base = map_.shape();
        d.start = unitAt(frameOf(s.id), pos);
        return d;
    };
    const Screen* s = map_.screen();
    const bool shown = s && (state.focus ? state.focus == s->id : !s->hidden);
    if (shown && state.item) {
        const BoardRect f = frameOf(s->id);
        const Points pts = map_.shape();
        for (size_t k = 0; k < pts.size(); ++k)
            if (pixelAt(f, pts[k]).getDistanceFrom(pos) < kGrab) {
                state.point = int(k);
                return start(Grab::Corner, int(k), *s);
            }
        for (size_t k = 0; map_.hasEdges() && k < pts.size(); ++k) {
            const auto m = (pixelAt(f, pts[k]) + pixelAt(f, pts[(k + 1) % pts.size()])) / 2.0f;
            if (m.getDistanceFrom(pos) < kGrab) {
                state.point = int(k);
                return start(Grab::Edge, int(k), *s);
            }
        }
        if (rotatePixel().toFloat().getDistanceFrom(pos) < kGrab)
            return start(Grab::Rotate, -1, *s);
    }
    if (auto title = grabTitle(e))
        return title;
    std::vector<const Screen*> screens = shownScreens();
    std::stable_partition(screens.begin(), screens.end(), [&](const Screen* x) { return x->id == state.screen; });
    for (const Screen* x : screens) {
        const BoardRect f = frameOf(x->id);
        const Point u = unitAt(f, pos);
        auto hit = [&](uint64_t item, bool mask, uint64_t owner) {
            if (!inside(outlineOf(*x, item, mask, owner), u))
                return false;
            if (x->id != state.screen || item != state.item || mask != state.mask)
                map_.select(x->id, item, mask);
            state.point = -1;
            return true;
        };
        if (x->id == state.screen && state.item && hit(state.item, state.mask, state.maskSlice))
            return start(Grab::Move, -1, *x);
        for (auto m = x->masks.rbegin(); m != x->masks.rend(); ++m)
            if (hit(m->id, true, 0))
                return start(Grab::Move, -1, *x);
        for (auto sl = x->slices.rbegin(); sl != x->slices.rend(); ++sl)
            for (auto m = sl->masks.rbegin(); m != sl->masks.rend(); ++m)
                if (hit(m->id, true, sl->id))
                    return start(Grab::Move, -1, *x);
        for (auto sl = x->slices.rbegin(); sl != x->slices.rend(); ++sl)
            if (hit(sl->id, false, 0))
                return start(Grab::Move, -1, *x);
    }
    return std::nullopt;
}

InputSnap MapCanvas::snapFor(uint64_t except, bool snap) const
{
    InputSnap out;
    out.snap = snap;
    const double scale = viewport().scale;
    out.reach = {kSnapPixels / (scale * kInputRect.w), kSnapPixels / (scale * kInputRect.h)};
    for (const Screen& s : session_.composition().screens)
        for (const Slice& slice : s.slices)
            if (slice.input == map_.input() && slice.id != except) {
                const auto& r = slice.inputRect;
                out.edgesX.insert(out.edgesX.end(), {r[0], r[0] + r[2]});
                out.edgesY.insert(out.edgesY.end(), {r[1], r[1] + r[3]});
            }
    return out;
}

std::optional<MapCanvas::Drag> MapCanvas::grabInput(const juce::MouseEvent& e)
{
    MapState& state = map_.state();
    const auto pos = e.position;
    const Point u = unitAt(kInputRect, pos);
    auto start = [&](Grab grab, int index) {
        Drag d;
        d.grab = grab;
        d.index = index;
        d.screen = state.screen;
        d.start = u;
        if (const Slice* slice = map_.slice())
            d.rect = slice->inputRect;
        return d;
    };
    if (const Slice* slice = map_.slice()) {
        const auto& r = slice->inputRect;
        const Point handles[] = {{r[0], r[1]}, {r[0] + r[2], r[1]}, {r[0] + r[2], r[1] + r[3]}, {r[0], r[1] + r[3]},
                                 {r[0] + r[2] / 2, r[1]}, {r[0] + r[2], r[1] + r[3] / 2}, {r[0] + r[2] / 2, r[1] + r[3]},
                                 {r[0], r[1] + r[3] / 2}};
        for (int k = 0; k < 8; ++k)
            if (pixelAt(kInputRect, handles[k]).getDistanceFrom(pos) < kGrab)
                return start(Grab::InputResize, k);
    }
    auto within = [&](const std::array<double, 4>& r) {
        return u[0] >= r[0] && u[0] <= r[0] + r[2] && u[1] >= r[1] && u[1] <= r[1] + r[3];
    };
    if (const Slice* slice = map_.slice(); slice && within(slice->inputRect) && !e.mods.isShiftDown())
        return start(Grab::InputMove, -1);
    const uint64_t input = map_.input();
    const auto& screens = session_.composition().screens;
    // The smallest rectangle under the hand wins, so one inside a full-input slice stays reachable.
    const Screen* hitScreen = nullptr;
    const Slice* hit = nullptr;
    for (const Screen& s : screens)
        for (const Slice& sl : s.slices)
            if (!e.mods.isShiftDown() && sl.input == input && within(sl.inputRect) &&
                (!hit || sl.inputRect[2] * sl.inputRect[3] < hit->inputRect[2] * hit->inputRect[3]))
                hitScreen = &s, hit = &sl;
    if (hit) {
        map_.select(hitScreen->id, hit->id, false);
        return start(Grab::InputMove, -1);
    }
    if (e.mods.isShiftDown() && map_.screen())
        return start(Grab::InputNew, -1);
    return std::nullopt;
}

void MapCanvas::mouseDown(const juce::MouseEvent& e)
{
    drag_ = map_.state().input ? grabInput(e) : grabOutput(e);
    if (drag_ && map_.state().locked) {
        drag_.reset();
        map_.state().point = -1;
    }
    if (!drag_) {
        drag_ = Drag{};
        drag_->grab = Grab::Pan;
    } else if (drag_->grab != Grab::InputNew) {
        session_.client().beginGesture();
    }
    drag_->startPixel = e.position;
    drag_->startView = viewport();
    lastSend_ = 0.0;
    map_.changed();
}

void MapCanvas::mouseDrag(const juce::MouseEvent& e)
{
    if (!drag_)
        return;
    if (!drag_->moved && e.position.getDistanceFrom(drag_->startPixel) < 2.0f)
        return;
    drag_->moved = true;
    if (drag_->grab == Grab::Pan) {
        Viewport v = drag_->startView;
        v.offset[0] += e.position.x - drag_->startPixel.x;
        v.offset[1] += e.position.y - drag_->startPixel.y;
        user_ = v;
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        repaint();
        return;
    }
    map_.state().input ? dragInput(e) : dragOutput(e);
    send(false);
    repaint();
}

void MapCanvas::dragOutput(const juce::MouseEvent& e)
{
    Drag& d = *drag_;
    if (d.grab == Grab::Frame) {
        const double scale = viewport().scale;
        liveFrame_ = {d.screen, {d.start[0] + (e.position.x - d.startPixel.x) / scale,
                                 d.start[1] + (e.position.y - d.startPixel.y) / scale}};
        return;
    }
    const BoardRect f = frameOf(d.screen);
    const Point u = unitAt(f, e.position);
    const Point delta{u[0] - d.start[0], u[1] - d.start[1]};
    const double aspect = kFrameW / kFrameH;
    switch (d.grab) {
    case Grab::Corner:
        livePoints_ = clamped(map_.movedPoint(d.base, d.index, delta));
        break;
    case Grab::Edge:
        livePoints_ = clamped(movedEdge(d.base, d.index, delta[0], delta[1]));
        break;
    case Grab::Move:
        livePoints_ = clamped(transformed(d.base, delta[0], delta[1]));
        break;
    case Grab::Rotate: {
        const Point c = centroid(map_.frameOf(d.base));
        auto iso = [&](Point q) { return Point{q[0] * aspect, q[1]}; };
        const double angle = sweep(iso(c), iso(d.start), iso(u), e.mods.isShiftDown());
        livePoints_ = clamped(transformed(d.base, 0.0, 0.0, 1.0, angle, aspect));
        break;
    }
    default:
        break;
    }
}

void MapCanvas::dragInput(const juce::MouseEvent& e)
{
    Drag& d = *drag_;
    const Point u = unitAt(kInputRect, e.position);
    if (d.grab == Grab::InputNew) {
        const double x0 = std::clamp(std::min(u[0], d.start[0]), 0.0, 1.0), x1 = std::clamp(std::max(u[0], d.start[0]), 0.0, 1.0);
        const double y0 = std::clamp(std::min(u[1], d.start[1]), 0.0, 1.0), y1 = std::clamp(std::max(u[1], d.start[1]), 0.0, 1.0);
        newRect_ = std::array<double, 4>{x0, y0, x1 - x0, y1 - y0};
        return;
    }
    const uint64_t item = map_.state().item;
    liveRect_ = draggedRect(d.rect, d.grab == Grab::InputMove ? -1 : d.index, {u[0] - d.start[0], u[1] - d.start[1]},
                            snapFor(item, !e.mods.isAltDown()));
}

void MapCanvas::send(bool force)
{
    if (!drag_ || (!force && seconds() - lastSend_ < kSendInterval))
        return;
    lastSend_ = seconds();
    const Drag& d = *drag_;
    std::optional<Point> target;
    if (livePoints_ && (d.grab == Grab::Corner || d.grab == Grab::Edge || d.grab == Grab::Move || d.grab == Grab::Rotate)) {
        map_.sendShape(*livePoints_);
        if (d.grab == Grab::Corner || d.grab == Grab::Edge)
            target = (*livePoints_)[size_t(d.index)];
    } else if (liveRect_ && (d.grab == Grab::InputMove || d.grab == Grab::InputResize)) {
        map_.sendInputRect(*liveRect_);
    } else if (liveFrame_ && d.grab == Grab::Frame) {
        session_.client().edit(SetScreenPosition{liveFrame_->first, liveFrame_->second});
    }
    if (target)
        map_.setLiveTarget(target);
}

void MapCanvas::mouseUp(const juce::MouseEvent& e)
{
    if (!drag_)
        return;
    const Drag d = *drag_;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    if (d.grab == Grab::Pan) {
        drag_.reset();
        if (!d.moved && !map_.state().input) {
            for (const Screen* s : shownScreens()) {
                const Point u = unitAt(frameOf(s->id), e.position);
                if (u[0] >= 0 && u[0] <= 1 && u[1] >= 0 && u[1] <= 1 && s->id != map_.state().screen)
                    return map_.select(s->id, 0, false);
            }
            map_.state().point = -1;
            map_.changed();
        }
        return;
    }
    if (d.grab == Grab::InputNew) {
        drag_.reset();
        const auto rect = newRect_;
        newRect_.reset();
        if (rect && (*rect)[2] >= 0.01 && (*rect)[3] >= 0.01)
            map_.addSlice(rect);
        repaint();
        return;
    }
    if (d.moved)
        send(true);
    session_.client().endGesture();
    drag_.reset();
    map_.setLiveTarget(std::nullopt);
    map_.changed();
}

void MapCanvas::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (map_.state().input)
        return;
    for (const Screen* s : shownScreens()) {
        const Point u = unitAt(frameOf(s->id), e.position);
        if (onTitle(s->id, e.position) || (u[0] >= 0 && u[0] <= 1 && u[1] >= 0 && u[1] <= 1))
            return map_.setFocus(map_.state().focus ? 0 : s->id);
    }
}

void MapCanvas::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const double delta = wheel.deltaY != 0.0f ? wheel.deltaY : wheel.deltaX;
    if (delta == 0.0)
        return;
    const double fit = fitted(board(), std::max(1, getWidth()), std::max(1, getHeight())).scale;
    user_ = zoomedAround(viewport(), fromJuce(e.position), std::exp(delta * 1.2), fit / 1.5, fit * 25.0);
    repaint();
    map_.repaint();
}

} // namespace azl::ui
