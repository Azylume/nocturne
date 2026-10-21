#include "ui/app/colour_picker.hpp"

#include "ui/app/look.hpp"

#include <cmath>

namespace azl::ui {

namespace {

constexpr double kSendInterval = 1.0 / 30.0;
constexpr int kPad = 8;
constexpr int kStrip = 14;
constexpr int kSquareHeight = 120;
constexpr int kRecent = 14;
constexpr int kRecentGap = 3;
constexpr size_t kPerRow = 12;
constexpr int kLabel = 14;

double seconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

juce::Colour toJuce(const Rgba& c)
{
    return juce::Colour::fromFloatRGBA(float(c.r), float(c.g), float(c.b), float(c.a));
}

double fraction(int at, int start, int length)
{
    return std::clamp(double(at - start) / std::max(1, length - 1), 0.0, 1.0);
}

// A grey or black tells nothing of its hue (or black of its saturation), so the picker keeps what it showed.
Hsva keepHue(Hsva fresh, const Hsva& shown)
{
    if (fresh.s <= 1e-6 || fresh.v <= 1e-6)
        fresh.h = shown.h;
    if (fresh.v <= 1e-6)
        fresh.s = shown.s;
    return fresh;
}

} // namespace

std::optional<Hsva> liveColour(const Session& session, const ColourRef& ref)
{
    const Composition& composition = session.composition();
    auto base = colourOf(composition, ref);
    if (!base)
        return std::nullopt;
    const std::vector<ParamRef> params = colourParams(composition, ref);
    bool driven = false;
    for (const ParamRef& p : params)
        driven = driven || session.drivenValue(p).has_value();
    if (!driven)
        return base;
    if (ref.effect) {
        const Effect* effect = effectById(composition, ref.effect);
        const EffectDef* def = effect ? findEffect(effect->type) : nullptr;
        auto group = def ? colourGroup(*def, ref.group) : std::nullopt;
        if (!group)
            return base;
        std::map<std::string, double> values = effect->params;
        for (const ParamRef& p : params)
            if (auto v = session.drivenValue(p))
                values[p.param] = *v;
        return keepHue(groupColour(*def, *group, values), *base);
    }
    Rgba rgb = toRgba(*base);
    double* channels[] = {&rgb.r, &rgb.g, &rgb.b, &rgb.a};
    for (size_t i = 0; i < params.size() && i < 4; ++i)
        if (auto v = session.drivenValue(params[i]))
            *channels[i] = *v;
    return keepHue(toHsva(rgb), *base);
}

void paintSwatch(juce::Graphics& g, juce::Rectangle<int> area, const Rgba& colour)
{
    if (colour.a < 1.0) {
        const int cell = std::max(3, area.getHeight() / 3);
        for (int y = area.getY(); y < area.getBottom(); y += cell)
            for (int x = area.getX(); x < area.getRight(); x += cell) {
                g.setColour(((x - area.getX()) / cell + (y - area.getY()) / cell) % 2 ? juce::Colour(0xff9a9a9a)
                                                                                    : juce::Colour(0xff5a5a5a));
                g.fillRect(juce::Rectangle<int>(x, y, cell, cell).getIntersection(area));
            }
    }
    g.setColour(toJuce(colour));
    g.fillRect(area);
    g.setColour(Look::palette().line);
    g.drawRect(area, 1);
}

ColourPicker::ColourPicker(Session& session, ColourRef ref)
    : session_(session), ref_(std::move(ref)), alpha_(hasAlpha(session.composition(), ref_)),
      colour_(colourOf(session.composition(), ref_).value_or(Hsva{}))
{
    hex_.setFont(Look::font());
    hex_.setIndents(4, 3);
    hex_.setJustification(juce::Justification::centredLeft);
    hex_.setTooltip("The colour as hex, #rrggbb" + juce::String(alpha_ ? " or #rrggbbaa" : "") + ": type and press Enter");
    hex_.onReturnKey = [this] { typed(); };
    hex_.onFocusLost = [this] { typed(); };
    addAndMakeVisible(hex_);
    view_.setTooltip("Switch between the picker and the colour's sliders");
    view_.onClick = [this] { showSliders(!sliders_); };
    addAndMakeVisible(view_);
    unlink_.setTooltip("Stop following the palette colour; the colour stays as it is");
    unlink_.onClick = [this] { session_.client().edit(SetPaletteLink{ref_, std::nullopt}); };
    addChildComponent(unlink_);
    session_.addView(this);
    follow();
    layout();
}

ColourRef ColourPicker::target() const
{
    const Composition& composition = session_.composition();
    const auto link = linkOf(composition, ref_);
    size_t slot = 0;
    if (!link || !paletteColour(composition, *link, &slot))
        return ref_;
    return {.group = kPaletteSlot, .palette = activePalette(composition)->id, .slot = slot};
}

bool ColourPicker::linkable() const
{
    return ref_.group != kPaletteSlot;
}

void ColourPicker::sendColour(const Hsva& colour)
{
    const ColourRef to = target();
    Hsva out = colour;
    if (!alpha_ && to.group == kPaletteSlot)
        if (const auto slot = colourOf(session_.composition(), to))
            out.a = slot->a;
    session_.setColour(to, out);
}

ColourPicker::~ColourPicker()
{
    if (dragging_ != Part::None)
        session_.client().endGesture();
    session_.removeView(this);
}

void ColourPicker::sessionChanged(int changes)
{
    if (changes & Session::Document) {
        follow();
        layout();
    }
    if (changes & (Session::Document | Session::Modulation))
        for (auto& row : rows_)
            row->refresh();
    if (changes & Session::Panels)
        repaint();
}

void ColourPicker::follow()
{
    if (dragging_ != Part::None)
        return;
    if (auto c = colourOf(session_.composition(), target()))
        colour_ = keepHue(*c, colour_);
    if (!hex_.hasKeyboardFocus(true))
        hex_.setText(toHex(toRgba(colour_), alpha_), false);
    unlink_.setVisible(linkOf(session_.composition(), ref_).has_value());
    for (size_t i = 0; i < fields_.size(); ++i) {
        const Rgba rgb = toRgba(colour_);
        const double values[] = {rgb.r, rgb.g, rgb.b, rgb.a};
        fields_[i]->setValue(values[i] * 100.0);
    }
    repaint();
}

void ColourPicker::showSliders(bool on)
{
    sliders_ = on;
    view_.setButtonText(on ? "Picker" : "Sliders");
    rows_.clear();
    fields_.clear();
    if (on)
        buildSliders();
    follow();
    layout();
}

void ColourPicker::buildSliders()
{
    if (ref_.effect) {
        for (const ParamRef& p : colourParams(session_.composition(), ref_)) {
            rows_.push_back(std::make_unique<ParamRow>(session_, p));
            addAndMakeVisible(*rows_.back());
        }
        return;
    }
    const char* names[] = {"R", "G", "B", "A"};
    for (int i = 0; i < (alpha_ ? 4 : 3); ++i) {
        auto field = std::make_unique<NumberField>(session_, 0.0, 100.0, 1.0, "%");
        field->setTooltip(juce::String(names[i]) + ": drag sideways or double-click to type");
        field->onChange = [this, i](double v) {
            Rgba rgb = toRgba(colour_);
            double* channels[] = {&rgb.r, &rgb.g, &rgb.b, &rgb.a};
            *channels[i] = v / 100.0;
            colour_ = keepHue(toHsva(rgb), colour_);
            sendColour(colour_);
        };
        fields_.push_back(std::move(field));
        addAndMakeVisible(*fields_.back());
    }
}

juce::Rectangle<int> ColourPicker::squareArea() const
{
    if (sliders_)
        return {};
    return {kPad, kPad, kWidth - 2 * kPad - kStrip - 6, kSquareHeight};
}

juce::Rectangle<int> ColourPicker::hueArea() const
{
    if (sliders_)
        return {};
    return {kWidth - kPad - kStrip, kPad, kStrip, kSquareHeight};
}

juce::Rectangle<int> ColourPicker::alphaArea() const
{
    if (sliders_ || !alpha_)
        return {};
    return {kPad, kPad + kSquareHeight + 6, kWidth - 2 * kPad, 10};
}

juce::Rectangle<int> ColourPicker::controlsRow() const
{
    int top = kPad;
    if (sliders_)
        top += int(rows_.size() * size_t(ParamRow::kHeight) + fields_.size() * 22) + 4;
    else
        top = (alpha_ ? alphaArea().getBottom() : squareArea().getBottom()) + 6;
    return {kPad, top, kWidth - 2 * kPad, 20};
}

int ColourPicker::recentTop() const
{
    return controlsRow().getBottom() + 6;
}

juce::Rectangle<int> ColourPicker::recentArea(size_t index) const
{
    return {kPad + int(index) * (kRecent + kRecentGap), recentTop(), kRecent, kRecent};
}

void ColourPicker::layout()
{
    int y = kPad;
    for (auto& row : rows_) {
        row->setBounds(0, y, kWidth, ParamRow::kHeight);
        y += ParamRow::kHeight;
    }
    for (auto& field : fields_) {
        field->setBounds(kPad + 24, y + 1, kWidth - 2 * kPad - 24, 20);
        y += 22;
    }
    auto row = controlsRow();
    row.removeFromLeft(28 + 4);
    hex_.setBounds(row.removeFromLeft(96));
    row.removeFromLeft(4);
    view_.setBounds(row);
    unlink_.setBounds(kWidth - kPad - 52, paletteTop() - kLabel - 1, 52, kLabel);
    const size_t rows = (paletteCells() + kPerRow - 1) / kPerRow;
    setSize(kWidth, paletteTop() + int(rows) * (kRecent + kRecentGap) + kPad);
    repaint();
}

int ColourPicker::paletteTop() const
{
    return recentTop() + kRecent + 6 + kLabel;
}

size_t ColourPicker::paletteCells() const
{
    const ColourPalette* palette = activePalette(session_.composition());
    const size_t count = palette ? palette->colours.size() : 0;
    return std::min(kMaxPaletteColours, (count / kPerRow + 1) * kPerRow);
}

juce::Rectangle<int> ColourPicker::paletteArea(size_t index) const
{
    return {kPad + int(index % kPerRow) * (kRecent + kRecentGap),
            paletteTop() + int(index / kPerRow) * (kRecent + kRecentGap), kRecent, kRecent};
}

juce::Rectangle<int> ColourPicker::previewArea() const
{
    return controlsRow().withWidth(28);
}

std::optional<size_t> ColourPicker::paletteCellAt(juce::Point<int> point) const
{
    for (size_t i = 0; i < paletteCells(); ++i)
        if (paletteArea(i).expanded(1).contains(point))
            return i;
    return std::nullopt;
}

void ColourPicker::editPalette(size_t slot, std::optional<std::array<double, 4>> colour)
{
    const ColourPalette* active = activePalette(session_.composition());
    if (!active || slot >= active->colours.size())
        return;
    ColourPalette palette = *active;
    if (colour)
        palette.colours[slot].colour = *colour;
    else
        palette.colours.erase(palette.colours.begin() + ptrdiff_t(slot));
    session_.client().edit(SetPalette{palette});
}

Menu ColourPicker::paletteMenu(size_t slot)
{
    const ColourPalette* palette = activePalette(session_.composition());
    const bool filled = palette && slot < palette->colours.size();
    const auto link = linkOf(session_.composition(), ref_);
    const uint64_t id = filled ? palette->colours[slot].id : 0;
    return {{.label = "Link to this colour",
             .run = [this, id] { session_.client().edit(SetPaletteLink{ref_, id}); },
             .enabled = filled && linkable() && link != id,
             .ticked = filled && link == id,
             .tip = "This colour follows the palette colour: changing it recolours everything linked to it"},
            {.label = "Unlink",
             .run = [this] { session_.client().edit(SetPaletteLink{ref_, std::nullopt}); },
             .enabled = link.has_value(),
             .tip = "Stop following the palette colour; the colour stays as it is"},
            {},
            {.label = "Update with current colour",
             .run = [this, slot] { editPalette(slot, arrayOf(toRgba(colour_))); },
             .enabled = filled,
             .tip = "This palette colour becomes the colour being picked"},
            {.label = "Remove",
             .run = [this, slot] { editPalette(slot, std::nullopt); },
             .enabled = filled,
             .tip = "Take this colour out of the palette"},
            {.label = "Clear all",
             .run = [this] {
                 if (const ColourPalette* p = activePalette(session_.composition())) {
                     ColourPalette cleared = *p;
                     cleared.colours.clear();
                     session_.client().edit(SetPalette{cleared});
                 }
             },
             .enabled = palette && !palette->colours.empty(),
             .tip = "Empty the palette; undo brings the colours back"}};
}

void ColourPicker::resized()
{
    if (getWidth() != kWidth)
        layout();
}

void ColourPicker::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.panel);
    if (!sliders_) {
        const auto square = squareArea();
        if (squareHue_ != colour_.h || !square_.isValid()) {
            squareHue_ = colour_.h;
            square_ = juce::Image(juce::Image::RGB, 48, 48, false);
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 48; ++x)
                    square_.setPixelAt(x, y, toJuce(toRgba({colour_.h, x / 47.0, 1.0 - y / 47.0, 1.0})));
        }
        g.drawImage(square_, square.toFloat());
        const float sx = float(square.getX()) + float(colour_.s) * float(square.getWidth() - 1);
        const float sy = float(square.getY()) + float(1.0 - colour_.v) * float(square.getHeight() - 1);
        g.setColour(colour_.v > 0.5 ? juce::Colours::black : juce::Colours::white);
        g.drawEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre({sx, sy}), 1.5f);

        const auto hue = hueArea();
        for (int y = 0; y < hue.getHeight(); ++y) {
            g.setColour(toJuce(toRgba({fraction(hue.getY() + y, hue.getY(), hue.getHeight()), 1.0, 1.0, 1.0})));
            g.fillRect(hue.getX(), hue.getY() + y, hue.getWidth(), 1);
        }
        const int hy = hue.getY() + juce::roundToInt(colour_.h * (hue.getHeight() - 1));
        g.setColour(p.white);
        g.drawRect(hue.getX() - 1, hy - 2, hue.getWidth() + 2, 4, 1);

        if (alpha_) {
            const auto a = alphaArea();
            paintSwatch(g, a, {0.5, 0.5, 0.5, 0.0});
            Rgba solid = toRgba(colour_);
            solid.a = 1.0;
            g.setGradientFill(juce::ColourGradient(toJuce(solid).withAlpha(0.0f), a.toFloat().getTopLeft(), toJuce(solid),
                                                   a.toFloat().getTopRight(), false));
            g.fillRect(a);
            const int ax = a.getX() + juce::roundToInt(colour_.a * (a.getWidth() - 1));
            g.setColour(p.white);
            g.drawRect(ax - 2, a.getY() - 1, 4, a.getHeight() + 2, 1);
        }
    }
    g.setFont(Look::smallFont());
    g.setColour(p.muted);
    const char* names[] = {"R", "G", "B", "A"};
    for (size_t i = 0; i < fields_.size(); ++i)
        drawText(g, names[i], fields_[i]->getBounds().withX(kPad).withWidth(20), juce::Justification::centredLeft);
    paintSwatch(g, controlsRow().withWidth(28), toRgba(colour_));
    const auto& recent = session_.recentColours();
    for (size_t i = 0; i < recent.size(); ++i)
        paintSwatch(g, recentArea(i), recent[i]);
    if (recent.empty())
        drawText(g, "Recent colours show here", juce::Rectangle<int>(kPad, recentTop(), kWidth - 2 * kPad, kRecent),
                 juce::Justification::centredLeft);

    const Composition& composition = session_.composition();
    const ColourPalette* palette = activePalette(composition);
    const ColourRef to = target();
    const bool linked = to.group == kPaletteSlot && !(to == ref_);
    juce::String title = palette ? utf8(palette->name) : juce::String("Palette: drag the colour onto a swatch");
    if (linked)
        title = "Linked to " + utf8(colourLabel(composition, to));
    g.setColour(linked ? p.accent : p.muted);
    drawText(g, title, juce::Rectangle<int>(kPad, paletteTop() - kLabel, kWidth - 2 * kPad - 56, kLabel - 2),
             juce::Justification::bottomLeft);
    for (size_t i = 0; i < paletteCells(); ++i) {
        const auto cell = paletteArea(i);
        if (palette && i < palette->colours.size()) {
            paintSwatch(g, cell, rgbaOf(palette->colours[i].colour));
            if (linked && to.slot == i) {
                g.setColour(p.accent);
                g.drawRect(cell.expanded(2), 2);
            }
            continue;
        }
        g.setColour(p.field);
        g.fillRect(cell);
        g.setColour(carried_ && cell.expanded(1).contains(*carried_) ? p.accent : p.button);
        g.drawRect(cell, 1);
    }
    if (carried_)
        paintSwatch(g, juce::Rectangle<int>(kRecent, kRecent).withCentre(*carried_), toRgba(colour_));
}

ColourPicker::Part ColourPicker::partAt(juce::Point<int> point) const
{
    if (squareArea().expanded(3).contains(point))
        return Part::Square;
    if (hueArea().expanded(3).contains(point))
        return Part::Hue;
    if (alphaArea().expanded(0, 3).contains(point))
        return Part::Alpha;
    return Part::None;
}

void ColourPicker::dragTo(juce::Point<int> point)
{
    if (dragging_ == Part::Square) {
        const auto r = squareArea();
        colour_.s = fraction(point.x, r.getX(), r.getWidth());
        colour_.v = 1.0 - fraction(point.y, r.getY(), r.getHeight());
    } else if (dragging_ == Part::Hue) {
        const auto r = hueArea();
        colour_.h = fraction(point.y, r.getY(), r.getHeight());
    } else if (dragging_ == Part::Alpha) {
        const auto r = alphaArea();
        colour_.a = fraction(point.x, r.getX(), r.getWidth());
    }
    hex_.setText(toHex(toRgba(colour_), alpha_), false);
    send(false);
    repaint();
}

void ColourPicker::send(bool force)
{
    if (!force && seconds() - lastSend_ < kSendInterval)
        return;
    lastSend_ = seconds();
    sendColour(colour_);
}

void ColourPicker::mouseDown(const juce::MouseEvent& e)
{
    const auto at = e.getPosition();
    const ColourPalette* palette = activePalette(session_.composition());
    const auto cell = paletteCellAt(at);
    const bool filled = cell && palette && *cell < palette->colours.size();
    if (e.mods.isPopupMenu()) {
        if (cell)
            showMenu(*this, paletteMenu(*cell));
        return;
    }
    if (filled && linkOf(session_.composition(), ref_))
        return session_.client().edit(SetPaletteLink{ref_, palette->colours[*cell].id});
    if (filled)
        return pick(keepHue(toHsva(rgbaOf(palette->colours[*cell].colour)), colour_));
    if (previewArea().contains(at)) {
        dragging_ = Part::Preview;
        return;
    }
    dragging_ = partAt(at);
    if (dragging_ != Part::None) {
        session_.client().beginGesture();
        lastSend_ = 0.0;
        dragTo(at);
        return;
    }
    const auto& recent = session_.recentColours();
    for (size_t i = 0; i < recent.size(); ++i)
        if (recentArea(i).contains(at))
            return pick(keepHue(toHsva(recent[i]), colour_));
}

void ColourPicker::mouseDrag(const juce::MouseEvent& e)
{
    if (dragging_ == Part::Preview) {
        carried_ = e.getPosition();
        repaint();
        return;
    }
    if (dragging_ != Part::None)
        dragTo(e.getPosition());
}

void ColourPicker::mouseUp(const juce::MouseEvent& e)
{
    if (dragging_ == Part::Preview) {
        dragging_ = Part::None;
        carried_.reset();
        repaint();
        const ColourPalette* palette = activePalette(session_.composition());
        const auto cell = paletteCellAt(e.getPosition());
        if (cell && *cell >= (palette ? palette->colours.size() : 0))
            session_.client().edit(addToPalette(session_.composition(),
                                                {nextColourName(palette), arrayOf(toRgba(colour_))}));
        return;
    }
    if (dragging_ == Part::None)
        return;
    send(true);
    session_.client().endGesture();
    dragging_ = Part::None;
    session_.addRecentColour(toRgba(colour_));
}

juce::String ColourPicker::tipAt(juce::Point<int> point)
{
    const ColourPalette* palette = activePalette(session_.composition());
    if (const auto cell = paletteCellAt(point)) {
        if (palette && *cell < palette->colours.size())
            return utf8(palette->colours[*cell].name) + ": click to pick it, right-click to update or remove it";
        return "An empty palette swatch: drag the colour on the left of the hex field here to add it";
    }
    if (previewArea().contains(point))
        return "The colour: drag it onto an empty palette swatch to add it to the palette";
    switch (partAt(point)) {
    case Part::Square: return "Saturation across, brightness up: drag to pick";
    case Part::Hue: return "Hue: drag to pick";
    case Part::Alpha: return "Alpha: drag to pick";
    default: break;
    }
    for (size_t i = 0; i < session_.recentColours().size(); ++i)
        if (recentArea(i).contains(point))
            return "A colour picked lately: click to pick it again";
    return {};
}

void ColourPicker::pick(const Hsva& colour)
{
    colour_ = colour;
    if (!alpha_)
        colour_.a = 1.0;
    hex_.setText(toHex(toRgba(colour_), alpha_), false);
    sendColour(colour_);
    session_.addRecentColour(toRgba(colour_));
    repaint();
}

void ColourPicker::typed()
{
    const std::string text = hex_.getText().toStdString();
    auto rgb = fromHex(text);
    if (!rgb) {
        hex_.setText(toHex(toRgba(colour_), alpha_), false);
        return;
    }
    const size_t digits = hex_.getText().retainCharacters("0123456789abcdefABCDEF").length();
    if (digits != 8)
        rgb->a = colour_.a;
    if (toHex(*rgb, true) == toHex(toRgba(colour_), true))
        return;
    pick(keepHue(toHsva(*rgb), colour_));
}

Swatch::Swatch(Session& session, ColourRef ref) : session_(session), ref_(std::move(ref))
{
    setTooltip("Click to pick the colour");
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    refresh();
}

Swatch::~Swatch()
{
    dropPicker(box_.get());
}

bool Swatch::refresh()
{
    const auto live = liveColour(session_, ref_);
    const std::optional<Rgba> shown = live ? std::optional<Rgba>(toRgba(*live)) : std::nullopt;
    if (shown == shown_)
        return false;
    shown_ = shown;
    repaint();
    return true;
}

void Swatch::paint(juce::Graphics& g)
{
    if (shown_)
        paintSwatch(g, getLocalBounds(), *shown_);
}

void Swatch::mouseUp(const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu() && getLocalBounds().contains(e.getPosition()))
        openPicker();
}

void Swatch::openPicker()
{
    dropPicker(box_.get());
    juce::Component* parent = getTopLevelComponent();
    picker_ = std::make_unique<ColourPicker>(session_, ref_);
    box_ = std::make_unique<juce::CallOutBox>(*picker_, parent->getLocalArea(this, getLocalBounds()), parent);
    const juce::CallOutBox* box = box_.get();
    box_->enterModalState(true, juce::ModalCallbackFunction::create([safe = SafePointer<Swatch>(this), box](int) {
        juce::MessageManager::callAsync([safe, box] {
            if (safe)
                safe->dropPicker(box);
        });
    }));
}

void Swatch::closePicker()
{
    if (box_)
        box_->dismiss();
}

void Swatch::dropPicker(const juce::CallOutBox* box)
{
    if (!box_ || box_.get() != box)
        return;
    if (box_->isCurrentlyModal())
        box_->exitModalState(0);
    box_.reset();
    picker_.reset();
}

SwatchRow::SwatchRow(Session& session, ColourRef ref) : session_(session), swatch_(session, std::move(ref))
{
    addAndMakeVisible(swatch_);
}

void SwatchRow::resized()
{
    swatch_.setBounds(84, 4, std::max(10, getWidth() - 84 - 8 - 62), kHeight - 8);
}

void SwatchRow::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const Composition& composition = session_.composition();
    g.setFont(Look::font());
    g.setColour(p.muted);
    drawText(g, utf8(colourLabel(composition, swatch_.ref())), juce::Rectangle<int>(8, 0, 72, kHeight),
             juce::Justification::centredLeft);
    const juce::Rectangle<int> value(getWidth() - 8 - 60, 0, 60, kHeight);
    const auto link = linkOf(composition, swatch_.ref());
    if (const PaletteColour* followed = link ? paletteColour(composition, *link) : nullptr) {
        g.setColour(p.accent);
        drawText(g, utf8(followed->name), value, juce::Justification::centredRight);
    } else if (const auto shown = swatch_.shown()) {
        g.setColour(p.text);
        drawText(g, utf8(toHex(*shown, hasAlpha(composition, swatch_.ref()) && shown->a < 1.0)), value,
                 juce::Justification::centredRight);
    }
}

namespace {

constexpr int kPaletteRow = 22;
constexpr int kTitle = 20;
constexpr int kNewPalette = 18;
constexpr int kLive = 6;

} // namespace

PaletteView::PaletteView(Session& session) : session_(session), pilot_(session, kPaletteEffect)
{
    addChildComponent(pilot_);
    add_.setTooltip("Add a colour to the palette: the colour picked last, else white");
    add_.onClick = [this] {
        const Composition& composition = session_.composition();
        const auto& recent = session_.recentColours();
        const Rgba colour = recent.empty() ? Rgba{1.0, 1.0, 1.0, 1.0} : recent.front();
        session_.client().edit(addToPalette(composition, {nextColourName(activePalette(composition)), arrayOf(colour)}));
    };
    addAndMakeVisible(add_);
    session_.addView(this);
    rebuild();
}

PaletteView::~PaletteView()
{
    session_.removeView(this);
}

void PaletteView::sessionChanged(int changes)
{
    if (changes & Session::Clock) {
        const ClockState clock = session_.client().clock();
        if (clock.palette != shown_ || clock.paletteColours != live_) {
            shown_ = clock.palette;
            live_ = clock.paletteColours;
            repaint();
        }
    }
    if (!(changes & Session::Document))
        return;
    rebuild();
    for (auto& swatch : swatches_)
        swatch->refresh();
    pilot_.repaint();
    repaint();
}

void PaletteView::rebuild()
{
    const Composition& composition = session_.composition();
    const ColourPalette* palette = activePalette(composition);
    std::string shape = palette ? std::to_string(palette->id) + "/" + std::to_string(palette->colours.size()) : "";
    for (const ColourPalette& p : composition.palettes)
        shape += "/" + p.name;
    if (shape == shape_)
        return;
    shape_ = shape;
    swatches_.clear();
    for (size_t i = 0; palette && i < palette->colours.size(); ++i) {
        swatches_.push_back(std::make_unique<Swatch>(session_, ColourRef{.group = kPaletteSlot, .palette = palette->id,
                                                                          .slot = i}));
        addAndMakeVisible(*swatches_.back());
    }
    resized();
}

juce::Rectangle<int> PaletteView::chipArea(size_t palette) const
{
    const std::vector<ColourPalette>& palettes = session_.composition().palettes;
    int x = 0;
    for (size_t i = 0; i < palettes.size(); ++i) {
        const int width =
            std::clamp(juce::GlyphArrangement::getStringWidthInt(Look::smallFont(), utf8(palettes[i].name)) + 12, 28, 96);
        if (i == palette)
            return {x, 2, width, kTitle - 4};
        x += width + 2;
    }
    return {x, 2, 0, kTitle - 4};
}

juce::Rectangle<int> PaletteView::newPaletteArea() const
{
    return chipArea(session_.composition().palettes.size()).withWidth(kNewPalette);
}

juce::Rectangle<int> PaletteView::titleArea() const
{
    const Composition& composition = session_.composition();
    return composition.palettes.empty() ? juce::Rectangle<int>(0, 0, getWidth() - 84, kTitle)
                                        : chipArea(activePaletteIndex(composition));
}

juce::Rectangle<int> PaletteView::rowArea(size_t slot) const
{
    return {0, kTitle + 6 + int(slot) * kPaletteRow, getWidth(), kPaletteRow - 2};
}

juce::Rectangle<int> PaletteView::liveArea(size_t slot) const
{
    return rowArea(slot).withX(46).withWidth(kLive).reduced(0, 3);
}

juce::Rectangle<int> PaletteView::nameArea(size_t slot) const
{
    return rowArea(slot).withTrimmedLeft(58).withTrimmedRight(24);
}

juce::Rectangle<int> PaletteView::removeArea(size_t slot) const
{
    return rowArea(slot).removeFromRight(20);
}

void PaletteView::resized()
{
    add_.setBounds(getWidth() - 80, 0, 80, kTitle);
    pilot_.setVisible(!session_.composition().palettes.empty());
    pilot_.setBounds(getWidth() - 84 - PilotBadge::kWidth, 0, PilotBadge::kWidth, kTitle);
    for (size_t i = 0; i < swatches_.size(); ++i)
        swatches_[i]->setBounds(rowArea(i).withWidth(44).reduced(0, 2));
}

void PaletteView::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const Composition& composition = session_.composition();
    const ColourPalette* palette = activePalette(composition);
    if (!palette) {
        g.setFont(Look::font(true));
        g.setColour(p.muted);
        drawText(g, "No palette yet: + Colour starts one", titleArea().reduced(4, 0), juce::Justification::centredLeft);
    }
    const size_t active = activePaletteIndex(composition);
    g.setFont(Look::smallFont());
    for (size_t i = 0; palette && i < composition.palettes.size(); ++i) {
        const juce::Rectangle<int> chip = chipArea(i);
        const bool lit = shown_ && shown_->step == i;
        g.setColour(lit ? p.live : p.button);
        g.fillRect(chip);
        if (i == active || (shown_ && shown_->next == i)) {
            g.setColour(i == active ? p.white : p.accent);
            g.drawRect(chip);
        }
        g.setColour(lit ? p.liveInk : p.text);
        drawText(g, utf8(composition.palettes[i].name), chip.reduced(4, 0), juce::Justification::centred);
    }
    if (palette) {
        g.setColour(p.button);
        g.fillRect(newPaletteArea());
        g.setColour(p.muted);
        drawText(g, "+", newPaletteArea(), juce::Justification::centred);
    }
    g.setFont(Look::font());
    std::map<size_t, int> linked;
    for (const auto& [ref, id] : linkedColours(composition))
        if (const auto slot = linkSlot(composition, id))
            ++linked[*slot];
    for (size_t i = 0; palette && i < palette->colours.size(); ++i) {
        const PaletteColour& colour = palette->colours[i];
        if (i < live_.size())
            paintSwatch(g, liveArea(i), rgbaOf(live_[i]));
        g.setColour(p.text);
        drawText(g, utf8(colour.name), nameArea(i), juce::Justification::centredLeft);
        g.setColour(p.muted);
        const juce::String links = linked[i] ? juce::String(linked[i]) + " linked  " : juce::String();
        drawText(g, links + utf8(toHex(rgbaOf(colour.colour), colour.colour[3] < 1.0)),
                 nameArea(i).withTrimmedRight(4), juce::Justification::centredRight);
        drawText(g, juce::String::fromUTF8("\xc3\x97"), removeArea(i), juce::Justification::centred);
    }
}

void PaletteView::addPalette()
{
    const Composition& composition = session_.composition();
    const ColourPalette* active = activePalette(composition);
    if (!active)
        return;
    ColourPalette palette = *active;
    palette.id = 0;
    palette.name = "Palette " + std::to_string(composition.palettes.size() + 1);
    for (PaletteColour& colour : palette.colours)
        colour.id = 0;
    session_.client().edit(SetPalette{palette});
}

Menu PaletteView::macroMenu(const std::string& param)
{
    Menu macros;
    Session& session = session_;
    for (const Macro& macro : session_.composition().macros)
        macros.push_back({.label = utf8(macro.name.empty() ? "Macro " + std::to_string(macro.id) : macro.name),
                          .run = [&session, id = macro.id, param] {
                              session.client().edit(AddMacroTarget{id, std::nullopt, {{0, 0, param}}});
                          }});
    return macros;
}

Menu PaletteView::chipMenu(size_t index)
{
    const Composition& composition = session_.composition();
    if (index >= composition.palettes.size())
        return {};
    Session& session = session_;
    const uint64_t id = composition.palettes[index].id;
    Menu decks;
    for (const Deck& deck : composition.decks)
        decks.push_back({.label = utf8(deck.name),
                         .run = [&session, deck = deck.id, id, set = deck.palette == id] {
                             session.client().edit(SetDeckPalette{deck, set ? 0 : id});
                         },
                         .ticked = deck.palette == id});
    const Menu macros = macroMenu(std::string(kPalettePosition));
    return {{.label = "Rename",
             .run = [this, index] {
                 const auto& palettes = session_.composition().palettes;
                 if (index < palettes.size())
                     editInline(editor_, *this, chipArea(index).withWidth(std::max(chipArea(index).getWidth(), 120)),
                                utf8(palettes[index].name), [this, index](const juce::String& text) {
                                    const auto& now = session_.composition().palettes;
                                    if (index >= now.size() || text.isEmpty())
                                        return;
                                    ColourPalette palette = now[index];
                                    palette.name = text.toStdString();
                                    session_.client().edit(SetPalette{palette});
                                });
             }},
            {.label = "Remove palette",
             .run = [&session, id] { session.client().edit(RemovePalette{id}); },
             .enabled = composition.palettes.size() > 1},
            {.label = "Deck palette",
             .sub = decks,
             .tip = "Showing the deck selects this palette, so a deck switch brings its colours"},
            {.label = "Position to macro",
             .sub = macros,
             .enabled = !macros.empty(),
             .tip = "The macro moves through the palettes, blending neighbours, so its modulators animate them"}};
}

Menu PaletteView::colourMenu(size_t slot)
{
    const Menu macros = macroMenu(paletteHueParam(slot));
    return {{.label = "Hue to macro",
             .sub = macros,
             .enabled = !macros.empty(),
             .tip = "The macro turns this colour's hue in every palette, so its modulators animate it"}};
}

void PaletteView::rename(size_t slot, const std::string& name)
{
    const ColourPalette* active = activePalette(session_.composition());
    if (!active || slot >= active->colours.size() || name.empty())
        return;
    ColourPalette palette = *active;
    palette.colours[slot].name = name;
    session_.client().edit(SetPalette{palette});
}

void PaletteView::renamePalette(const std::string& name)
{
    const ColourPalette* active = activePalette(session_.composition());
    if (!active || name.empty())
        return;
    ColourPalette palette = *active;
    palette.name = name;
    session_.client().edit(SetPalette{palette});
}

void PaletteView::mouseUp(const juce::MouseEvent& e)
{
    const Composition& composition = session_.composition();
    const ColourPalette* active = activePalette(composition);
    if (active && newPaletteArea().contains(e.getPosition()))
        return addPalette();
    for (size_t i = 0; active && i < composition.palettes.size(); ++i)
        if (chipArea(i).contains(e.getPosition())) {
            if (e.mods.isPopupMenu())
                return showMenu(*this, chipMenu(i));
            return session_.client().selectPalette(composition.palettes[i].id);
        }
    for (size_t i = 0; active && i < active->colours.size(); ++i)
        if (e.mods.isPopupMenu() && rowArea(i).contains(e.getPosition()))
            return showMenu(*this, colourMenu(i));
    for (size_t i = 0; active && i < active->colours.size(); ++i)
        if (removeArea(i).contains(e.getPosition())) {
            ColourPalette palette = *active;
            palette.colours.erase(palette.colours.begin() + ptrdiff_t(i));
            session_.client().edit(SetPalette{palette});
            return;
        }
}

void PaletteView::mouseDoubleClick(const juce::MouseEvent& e)
{
    const ColourPalette* active = activePalette(session_.composition());
    if (!active)
        return;
    if (titleArea().contains(e.getPosition()))
        return editInline(editor_, *this, titleArea(), utf8(active->name),
                          [this](const juce::String& text) { renamePalette(text.toStdString()); });
    for (size_t i = 0; i < active->colours.size(); ++i)
        if (nameArea(i).contains(e.getPosition()))
            return editInline(editor_, *this, nameArea(i), utf8(active->colours[i].name),
                              [this, i](const juce::String& text) { rename(i, text.toStdString()); });
}

juce::String PaletteView::tipAt(juce::Point<int> point)
{
    const Composition& composition = session_.composition();
    const ColourPalette* active = activePalette(composition);
    if (titleArea().contains(point))
        return active ? "The active palette, which every colour picker offers: double-click to rename it; right-click "
                        "for more"
                      : juce::String();
    for (size_t i = 0; active && i < composition.palettes.size(); ++i)
        if (chipArea(i).contains(point))
            return "Click to crossfade every linked colour to this palette on the next beat; right-click to rename, "
                   "remove, make it a deck's palette or drive it with a macro";
    if (active && newPaletteArea().contains(point))
        return "Add a palette: a copy of this one's colours to change, slot by slot";
    for (size_t i = 0; active && i < active->colours.size(); ++i) {
        if (liveArea(i).contains(point))
            return "The colour the show draws now";
        if (removeArea(i).contains(point))
            return "Remove this colour from the palette";
        if (nameArea(i).contains(point))
            return "Double-click to rename the colour";
    }
    return {};
}

} // namespace azl::ui
