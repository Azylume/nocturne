#include "ui/app/widgets.hpp"

#include "ui/app/colour_picker.hpp"
#include "ui/app/look.hpp"
#include "ui/app/preset_tree.hpp"

#include <cmath>

namespace azl::ui {

namespace {

constexpr double kSendInterval = 1.0 / 30.0;
constexpr int kLabelWidth = 72;
constexpr int kValueWidth = 46;

double seconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

int decimalsFor(double step)
{
    return step >= 1.0 ? 0 : step >= 0.1 ? 1 : 2;
}

float xOf(juce::Rectangle<int> bar, const ParamInfo& info, double value)
{
    return float(bar.getX()) + float(bar.getWidth()) * float(std::clamp(info.fraction(info.fit(value)), 0.0, 1.0));
}

juce::String formatNumber(double value, double step)
{
    const int decimals = decimalsFor(step);
    return decimals == 0 ? juce::String(juce::roundToInt(value)) : juce::String(value, decimals);
}

} // namespace

namespace {

struct KeyedEditor : juce::TextEditor {
    std::function<bool(const juce::KeyPress&)> keys;
    bool keyPressed(const juce::KeyPress& key) override { return (keys && keys(key)) || juce::TextEditor::keyPressed(key); }
};

} // namespace

void editInline(std::unique_ptr<juce::TextEditor>& slot, juce::Component& parent, juce::Rectangle<int> area,
                const juce::String& text, std::function<void(const juce::String&)> done,
                std::function<bool(const juce::KeyPress&)> keys)
{
    auto keyed = std::make_unique<KeyedEditor>();
    keyed->keys = std::move(keys);
    slot = std::move(keyed);
    juce::TextEditor* editor = slot.get();
    parent.addAndMakeVisible(editor);
    editor->setBounds(area);
    editor->setFont(Look::font());
    editor->setIndents(4, 2);
    editor->setText(text, false);
    editor->selectAll();
    editor->grabKeyboardFocus();
    auto finished = std::make_shared<bool>(false);
    juce::Component::SafePointer<juce::Component> owner(&parent);
    auto finish = [&slot, editor, text, done, finished, owner](bool commit) {
        if (*finished)
            return;
        *finished = true;
        const juce::String value = editor->getText().trim();
        editor->setVisible(false);
        juce::MessageManager::callAsync([&slot, editor, owner] {
            if (owner && slot.get() == editor)
                slot.reset();
        });
        if (commit && value.isNotEmpty() && value != text)
            done(value);
    };
    editor->onReturnKey = [finish] { finish(true); };
    editor->onEscapeKey = [finish] { finish(false); };
    editor->onFocusLost = [finish] { finish(true); };
}

void askName(const juce::String& title, const juce::String& message, const juce::String& initial,
             std::function<void(const juce::String&)> done)
{
    auto* window = new juce::AlertWindow(title, message, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("name", initial);
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create([window, done](int result) {
                                const juce::String name = window->getTextEditorContents("name").trim();
                                if (result == 1 && name.isNotEmpty())
                                    done(name);
                            }),
                            true);
}

namespace {

// A menu item drawn as JUCE draws one, with a tooltip.
class TipItem : public juce::PopupMenu::CustomComponent, public juce::SettableTooltipClient {
public:
    explicit TipItem(const MenuEntry& entry)
        : text_((entry.mixed ? juce::String::fromUTF8("\xe2\x80\x93 ") : juce::String()) + entry.label),
          shortcut_(entry.shortcut), enabled_(entry.enabled), ticked_(entry.ticked), sub_(!entry.sub.empty())
    {
        setTooltip(entry.tip);
    }

    void getIdealSize(int& width, int& height) override
    {
        getLookAndFeel().getIdealPopupMenuItemSize(shortcut_.isEmpty() ? text_ : text_ + "   " + shortcut_, false, -1,
                                                   width, height);
    }

    void paint(juce::Graphics& g) override
    {
        getLookAndFeel().drawPopupMenuItem(g, getLocalBounds(), false, enabled_, isItemHighlighted() && enabled_, ticked_,
                                           sub_, text_, shortcut_, nullptr, nullptr);
    }

private:
    const juce::String text_, shortcut_;
    const bool enabled_, ticked_, sub_;
};

void addEntries(juce::PopupMenu& menu, const Menu& entries, std::vector<std::function<void()>>& actions)
{
    for (const MenuEntry& entry : entries) {
        if (!entry.run && entry.sub.empty()) {
            menu.addSeparator();
            continue;
        }
        juce::PopupMenu::Item item(entry.label);
        item.isEnabled = entry.enabled;
        item.isTicked = entry.ticked;
        item.shortcutKeyDescription = entry.shortcut;
        item.customComponent = new TipItem(entry);
        if (!entry.sub.empty()) {
            item.subMenu = std::make_unique<juce::PopupMenu>();
            addEntries(*item.subMenu, entry.sub, actions);
        } else {
            actions.push_back(entry.run);
            item.itemID = int(actions.size());
        }
        menu.addItem(std::move(item));
    }
}

} // namespace

// Shown inside the window, so the tooltip window (a child of it) shows the items' tips.
void showMenu(juce::Component& target, Menu entries)
{
    juce::PopupMenu menu;
    auto actions = std::make_shared<std::vector<std::function<void()>>>();
    addEntries(menu, entries, *actions);
    const auto options = juce::PopupMenu::Options().withTargetComponent(&target).withMousePosition().withParentComponent(
        target.getTopLevelComponent());
    menu.showMenuAsync(options, [actions](int result) {
        if (result > 0 && result <= int(actions->size()))
            (*actions)[size_t(result - 1)]();
    });
}

juce::String generatorLabel(const std::string& type)
{
    if (const EffectDef* def = findEffect(type); def && !def->label.empty())
        return utf8(def->label) + (def->category == "User" ? " (user)" : "");
    const juce::String name(type);
    return name.substring(0, 1).toUpperCase() + name.substring(1);
}

ClipRef generatorClip(const std::string& type)
{
    if (type == kTextSource)
        return {.name = "Text", .text = TextClip{.text = "TEXT"}};
    return {.name = generatorLabel(type).toStdString(), .effects = {Effect{0, type, {}}}};
}

void paintGenerator(juce::Graphics& g, juce::Rectangle<int> area, const Effect& generator)
{
    if (generator.type == kTextSource) {
        g.setColour(juce::Colours::black);
        g.fillRect(area);
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(float(area.getHeight()) * 0.7f, juce::Font::bold)));
        g.drawText("T", area, juce::Justification::centred, false);
        return;
    }
    const EffectDef* def = findEffect(generator.type);
    if (!def)
        return;
    auto value = [&](const std::string& name) {
        if (auto it = generator.params.find(name); it != generator.params.end())
            return it->second;
        for (const ParamDef& param : def->params)
            if (param.name == name)
                return param.defaultValue;
        return 0.0;
    };
    auto colour = [&](const std::string& red, const std::string& green, const std::string& blue) {
        return juce::Colour::fromFloatRGBA(float(value(red)), float(value(green)), float(value(blue)), 1.0f);
    };
    const juce::Colour ink = colour("red", "green", "blue");
    const auto r = area.toFloat();
    g.setColour(juce::Colours::black);
    g.fillRect(area);
    const std::string& type = generator.type;
    if (type == "blank") {
        g.setColour(juce::Colour(0xff3a3a3a));
        g.drawRect(area.reduced(4), 1);
        g.drawLine(r.getX() + 4, r.getBottom() - 4, r.getRight() - 4, r.getY() + 4, 1.0f);
    } else if (type == "solid") {
        g.setColour(ink);
        g.fillRect(area);
    } else if (type == "strobe") {
        g.setColour(ink);
        g.fillRect(r.withWidth(r.getWidth() * float(value("duty"))));
    } else if (type == "lines" || type == "checker") {
        const int count = std::clamp(int(std::lround(value("count"))), 1, 16);
        const float cell = r.getHeight() / float(count);
        g.setColour(ink);
        for (int j = 0; type == "checker" ? j < count : j < 1; ++j)
            for (int i = 0; float(i) * cell < r.getWidth(); ++i) {
                if (type == "lines")
                    g.fillRect(juce::Rectangle<float>(r.getX() + float(i) * cell, r.getY(), cell * float(value("width")),
                                                      r.getHeight()));
                else if ((i + j) % 2 == 0)
                    g.fillRect(juce::Rectangle<float>(r.getX() + float(i) * cell, r.getY() + float(j) * cell, cell, cell)
                                   .getIntersection(r));
            }
    } else if (type == "rings") {
        const int count = std::clamp(int(std::lround(value("count"))), 1, 12);
        const float step = r.getHeight() * 0.5f / float(count);
        const juce::Point<float> centre = r.getCentre() + juce::Point<float>(float(value("x")) * r.getWidth(),
                                                                             -float(value("y")) * r.getHeight());
        g.setColour(ink);
        g.saveState();
        g.reduceClipRegion(area);
        for (int i = 1; float(i) * step < r.getWidth(); ++i)
            g.drawEllipse(juce::Rectangle<float>(step * 2.0f * float(i), step * 2.0f * float(i)).withCentre(centre),
                          std::max(1.0f, step * float(value("width"))));
        g.restoreState();
    } else if (type == "gradient") {
        g.setGradientFill(juce::ColourGradient(ink, r.getTopLeft(), colour("toRed", "toGreen", "toBlue"),
                                               r.getBottomRight(), false));
        g.fillRect(area);
    } else if (type == "noise") {
        g.setGradientFill(juce::ColourGradient(ink, r.getCentre(), juce::Colours::black,
                                               r.getCentre() + juce::Point<float>(r.getWidth() * 0.4f, 0.0f), true));
        g.fillRect(area);
    }
}

Menu addEffectMenu(Session& session, const std::function<void(const Effect&)>& add)
{
    const std::vector<PresetEntry> presets = session.client().presetEntries();
    Menu menu;
    for (const std::string& type : effectTypes()) {
        const Effect plain = newEffect(session.client(), type);
        const juce::String tip = "Add " + juce::String(type) + " with its default settings";
        Menu sub{{.label = "Default", .run = [add, plain] { add(plain); }, .tip = tip}};
        for (const char* group : kPresetGroups) {
            Menu named;
            for (const PresetEntry& preset : presets)
                if (preset.effectType == type && preset.group == group)
                    named.push_back({.label = utf8(preset.name),
                                     .run = [add, effect = presetEffect(preset)] { add(effect); },
                                     .tip = "Add " + juce::String(type) + " with the preset " + utf8(preset.name)});
            if (!named.empty())
                sub.push_back({.label = presetGroupLabel(group), .sub = named, .tip = "Add it from one of these presets"});
        }
        if (sub.size() == 1)
            menu.push_back({.label = type, .run = [add, plain] { add(plain); }, .tip = tip});
        else
            menu.push_back({.label = type, .sub = sub, .tip = "Add " + juce::String(type) + ", plain or from a preset"});
    }
    return menu;
}

const MenuEntry* menuEntry(const Menu& menu, const juce::String& path)
{
    const juce::String first = path.upToFirstOccurrenceOf("/", false, false);
    for (const MenuEntry& entry : menu)
        if (entry.label == first)
            return path.contains("/") ? menuEntry(entry.sub, path.fromFirstOccurrenceOf("/", false, false)) : &entry;
    return nullptr;
}

int wheelScrolled(int offset, const juce::MouseWheelDetails& wheel, int content, int visible)
{
    const float pixels = wheel.deltaY * 240.0f;
    int step = juce::roundToInt(pixels);
    if (step == 0 && pixels != 0.0f)
        step = pixels > 0.0f ? 1 : -1;
    return juce::jlimit(0, std::max(0, content - visible), offset - step);
}

juce::String formatValue(const ParamInfo& info, double value)
{
    return utf8(info.format(value));
}

juce::String rangeText(const ParamInfo& info)
{
    return formatValue(info, info.min) + " to " + formatValue(info, info.max) + (info.wraps ? ", wrapping round" : "");
}

NumberField::NumberField(Session& session, double min, double max, double step, juce::String suffix)
    : session_(session), min_(min), max_(max), step_(step), suffix_(std::move(suffix))
{
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
}

void NumberField::setValue(std::optional<double> value)
{
    if (dragging_ || wheeling_ || editor_)
        return;
    mixed_ = !value;
    value_ = value.value_or(value_);
    repaint();
}

void NumberField::set(double value)
{
    value = std::clamp(std::round(value / step_) * step_, min_, max_);
    if (value == value_)
        return;
    value_ = value;
    repaint();
    if (onChange)
        onChange(value_);
}

HoldButton::HoldButton(juce::String text, double hold) : text_(std::move(text)), hold_(hold) {}

void HoldButton::setText(const juce::String& text)
{
    if (text == text_)
        return;
    text_ = text;
    repaint();
}

void HoldButton::press()
{
    if (!isEnabled())
        return;
    pressedAt_ = seconds();
    fired_ = false;
    startTimerHz(30);
    repaint();
}

void HoldButton::release()
{
    pressedAt_.reset();
    stopTimer();
    repaint();
}

double HoldButton::progress() const
{
    return pressedAt_ ? std::clamp((seconds() - *pressedAt_) / hold_, 0.0, 1.0) : 0.0;
}

void HoldButton::timerCallback()
{
    repaint();
    if (fired_ || progress() < 1.0)
        return;
    fired_ = true;
    stopTimer();
    if (onHold)
        onHold();
}

void HoldButton::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const auto area = getLocalBounds();
    g.setColour(p.button);
    g.fillRect(area);
    g.setColour(p.accent.withAlpha(0.6f));
    g.fillRect(area.withWidth(juce::roundToInt(float(area.getWidth()) * float(progress()))));
    g.setColour(isEnabled() ? p.white : p.muted);
    g.setFont(Look::font());
    drawText(g, text_, area.reduced(4, 0), juce::Justification::centred, true);
}

void HoldButton::mouseDown(const juce::MouseEvent&)
{
    press();
}

void HoldButton::mouseUp(const juce::MouseEvent&)
{
    release();
}

void HoldButton::enablementChanged()
{
    if (!isEnabled())
        release();
    repaint();
}

void NumberField::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setColour(p.field);
    g.fillRect(getLocalBounds());
    g.setColour(isEnabled() ? p.text : p.muted);
    g.setFont(Look::font());
    drawText(g, mixed_ ? juce::String("mixed") : formatNumber(value_, step_) + suffix_, getLocalBounds().reduced(4, 0),
               juce::Justification::centredRight, true);
}

void NumberField::mouseDown(const juce::MouseEvent&)
{
    dragStart_ = value_;
    dragging_ = false;
}

void NumberField::mouseDrag(const juce::MouseEvent& e)
{
    const int dx = e.getDistanceFromDragStartX();
    if (!dragging_ && std::abs(dx) < 3)
        return;
    if (!dragging_) {
        dragging_ = true;
        session_.client().beginGesture();
    }
    const double scale = e.mods.isShiftDown() ? 10.0 : 1.0;
    set(dragStart_ + std::round(dx / 3.0) * step_ * scale);
}

void NumberField::mouseUp(const juce::MouseEvent& e)
{
    if (!dragging_) {
        if (!e.mods.isPopupMenu() && !editor_ && isEnabled())
            startEditing();
        return;
    }
    session_.client().endGesture();
    dragging_ = false;
    if (onEnd)
        onEnd();
}

void NumberField::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& details)
{
    if (!wheel || !isEnabled()) {
        Component::mouseWheelMove(e, details);
        return;
    }
    wheelRest_ += (details.deltaY != 0.0f ? details.deltaY : -details.deltaX) * 10.0;
    const double steps = std::trunc(wheelRest_);
    if (steps == 0.0)
        return;
    wheelRest_ -= steps;
    if (!wheeling_) {
        wheeling_ = true;
        session_.client().beginGesture();
    }
    set(value_ + steps * step_ * (e.mods.isShiftDown() ? 10.0 : 1.0));
    startTimer(400);
}

void NumberField::timerCallback()
{
    stopTimer();
    if (!wheeling_)
        return;
    wheeling_ = false;
    session_.client().endGesture();
    if (onEnd)
        onEnd();
}

void NumberField::mouseDoubleClick(const juce::MouseEvent&)
{
    if (!editor_ && isEnabled())
        startEditing();
}

void NumberField::startEditing()
{
    editInline(
        editor_, *this, getLocalBounds(), formatNumber(value_, step_),
        [this](const juce::String& text) {
            set(text.getDoubleValue());
            if (onEnd)
                onEnd();
        },
        [this](const juce::KeyPress& key) { return stepKey(key); });
}

bool NumberField::stepKey(const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    if (!editor_ || (code != juce::KeyPress::upKey && code != juce::KeyPress::downKey))
        return false;
    const double direction = code == juce::KeyPress::upKey ? 1.0 : -1.0;
    set(value_ + direction * step_ * (key.getModifiers().isShiftDown() ? 10.0 : 1.0));
    editor_->setText(formatNumber(value_, step_), false);
    editor_->selectAll();
    if (onEnd)
        onEnd();
    return true;
}

void drawCurve(juce::Graphics& g, juce::Rectangle<float> area, const Curve& curve, juce::Colour colour)
{
    juce::Path path;
    for (int i = 0; i <= 32; ++i) {
        const double x = i / 32.0;
        const float px = area.getX() + float(x) * area.getWidth();
        const float py = area.getBottom() - float(std::clamp(curveAt(curve, x), -0.1, 1.1)) * area.getHeight();
        if (i == 0)
            path.startNewSubPath(px, py);
        else
            path.lineTo(px, py);
    }
    g.setColour(colour);
    g.strokePath(path, juce::PathStrokeType(1.5f));
}

CurveEditor::CurveEditor(Session& session, Curve curve, std::function<void(const Curve&)> changed)
    : session_(session), curve_(curve), changed_(std::move(changed))
{
    setSize(180, 150);
    setTooltip("Drag the two points to bend how the macro maps onto this target");
}

juce::Rectangle<float> CurveEditor::plot() const
{
    return getLocalBounds().toFloat().reduced(12.0f);
}

juce::Point<float> CurveEditor::handle(int i) const
{
    const auto r = plot();
    return {r.getX() + float(curve_.points[size_t(i * 2)]) * r.getWidth(),
            r.getBottom() - float(curve_.points[size_t(i * 2 + 1)]) * r.getHeight()};
}

void CurveEditor::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const auto r = plot();
    g.fillAll(p.panel);
    g.setColour(p.field);
    g.fillRect(r);
    g.setColour(p.muted.withAlpha(0.6f));
    g.drawLine(r.getX(), r.getBottom(), handle(0).x, handle(0).y);
    g.drawLine(r.getRight(), r.getY(), handle(1).x, handle(1).y);
    drawCurve(g, r, curve_, p.live);
    for (int i = 0; i < 2; ++i) {
        g.setColour(dragging_ == i ? p.white : p.accent);
        g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(handle(i)));
    }
}

void CurveEditor::mouseDown(const juce::MouseEvent& e)
{
    dragging_ = -1;
    float best = 14.0f;
    for (int i = 0; i < 2; ++i)
        if (const float d = handle(i).getDistanceFrom(e.position); d < best)
            best = d, dragging_ = i;
    if (dragging_ >= 0)
        session_.client().beginGesture();
    repaint();
}

void CurveEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (dragging_ < 0)
        return;
    const auto r = plot();
    curve_.points[size_t(dragging_ * 2)] = std::clamp(double((e.position.x - r.getX()) / r.getWidth()), 0.0, 1.0);
    curve_.points[size_t(dragging_ * 2 + 1)] = std::clamp(double((r.getBottom() - e.position.y) / r.getHeight()), 0.0, 1.0);
    repaint();
    changed_(curve_);
}

void CurveEditor::mouseUp(const juce::MouseEvent&)
{
    if (dragging_ >= 0)
        session_.client().endGesture();
    dragging_ = -1;
    repaint();
}

namespace {

// The narrowest gap between two points of a custom curve, also kept before the period's end.
constexpr double kPointGap = 0.002;
constexpr float kPointRadius = 4.0f;

std::vector<LfoPoint> pointsOf(LfoShape shape)
{
    const double end = 1.0 - kPointGap;
    switch (shape) {
    case LfoShape::Saw:
        return {{0.0, 1.0}, {end, 0.0}};
    case LfoShape::Ramp:
        return {{0.0, 0.0}, {end, 1.0}};
    case LfoShape::Square:
        return {{0.0, 1.0}, {0.5 - kPointGap, 1.0}, {0.5, 0.0}, {end, 0.0}};
    default:
        return {{0.0, 1.0, true}, {0.5, 0.0, true}};
    }
}

double lfoSensitivity(const Composition& composition)
{
    const auto it = composition.sensitivity.find("lfo");
    return it == composition.sensitivity.end() ? 1.0 : it->second;
}

} // namespace

void setLfoShape(Modulator& m, LfoShape shape)
{
    if (shape == LfoShape::Custom && m.points.empty())
        m.points = pointsOf(m.shape);
    m.shape = shape;
}

LfoEditor::LfoEditor(Session& session, std::function<const Modulator*()> modulator, Change change)
    : session_(session), modulator_(std::move(modulator)), change_(std::move(change))
{
    startTimerHz(30);
}

std::optional<Modulator> LfoEditor::shown() const
{
    const Modulator* m = modulator_();
    if (!m)
        return std::nullopt;
    Modulator shown = *m;
    if (dragging_)
        shown.points = dragPoints_;
    return shown;
}

juce::Rectangle<float> LfoEditor::plot() const
{
    return getLocalBounds().toFloat().reduced(kPointRadius + 1.0f);
}

double LfoEditor::plotAt(double x) const
{
    const auto m = shown();
    if (!m)
        return 0.0;
    const Composition& composition = session_.composition();
    const double source = std::clamp(lfoAt(m->shape, x, m->points) * lfoSensitivity(composition), 0.0, 1.0);
    return modulatedAt(composition, *m, source);
}

juce::Point<float> LfoEditor::pointPosition(size_t index) const
{
    const auto m = shown();
    if (!m || index >= m->points.size())
        return {};
    const Composition& composition = session_.composition();
    const LfoPoint& point = m->points[index];
    const double y = modulatedAt(composition, *m, std::clamp(point.value * lfoSensitivity(composition), 0.0, 1.0));
    const auto r = plot();
    return {r.getX() + r.getWidth() * float(point.phase), r.getBottom() - r.getHeight() * float(y)};
}

// The point value that plots nearest to `y`; of several that plot alike (past a clamp), the one nearest `previous`.
double LfoEditor::sourceAt(const Modulator& m, float y, double previous) const
{
    const Composition& composition = session_.composition();
    const auto r = plot();
    const double target = std::clamp(double((r.getBottom() - y) / r.getHeight()), 0.0, 1.0);
    const double sensitivity = lfoSensitivity(composition);
    double best = previous;
    double distance = std::numeric_limits<double>::max();
    for (int i = 0; i <= 500; ++i) {
        const double value = i / 500.0;
        const double d = std::abs(modulatedAt(composition, m, std::clamp(value * sensitivity, 0.0, 1.0)) - target);
        const bool tie = std::abs(d - distance) < 1e-9;
        if ((!tie && d < distance) || (tie && std::abs(value - previous) < std::abs(best - previous)))
            best = value, distance = d;
    }
    return best;
}

std::optional<size_t> LfoEditor::pointAt(juce::Point<float> position) const
{
    const auto m = shown();
    if (!m || m->shape != LfoShape::Custom)
        return std::nullopt;
    std::optional<size_t> found;
    float best = kPointRadius + 3.0f;
    for (size_t i = 0; i < m->points.size(); ++i)
        if (const float d = pointPosition(i).getDistanceFrom(position); d < best)
            best = d, found = i;
    return found;
}

void LfoEditor::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setColour(p.field);
    g.fillRect(getLocalBounds());
    const auto m = shown();
    if (!m)
        return;
    const auto r = plot();
    g.setColour(p.muted.withAlpha(0.3f));
    g.drawHorizontalLine(juce::roundToInt(r.getCentreY()), r.getX(), r.getRight());
    auto yAt = [&](double x) { return r.getBottom() - r.getHeight() * float(plotAt(x)); };
    const int steps = std::max(1, juce::roundToInt(r.getWidth()));
    juce::Path curve;
    curve.startNewSubPath(r.getX(), yAt(0.0));
    for (int i = 1; i <= steps; ++i)
        curve.lineTo(r.getX() + r.getWidth() * float(i) / float(steps), yAt(double(i) / steps));
    g.setColour(p.accent);
    g.strokePath(curve, juce::PathStrokeType(1.5f));
    if (m->shape == LfoShape::Custom)
        for (size_t i = 0; i < m->points.size(); ++i) {
            const auto dot = juce::Rectangle<float>(2.0f * kPointRadius, 2.0f * kPointRadius).withCentre(pointPosition(i));
            g.setColour(dragging_ == i ? p.white : p.text);
            if (m->points[i].smooth)
                g.fillEllipse(dot);
            else
                g.fillRect(dot.reduced(0.5f));
        }
    const double periods = session_.client().beatNow() / std::max(m->beats, kMinLfoBeats) + m->phase;
    const double head = periods - std::floor(periods);
    const float x = r.getX() + r.getWidth() * float(head);
    g.setColour(p.white.withAlpha(0.7f));
    g.drawVerticalLine(juce::roundToInt(x), r.getY(), r.getBottom());
    g.fillEllipse(x - 2.5f, yAt(head) - 2.5f, 5.0f, 5.0f);
}

void LfoEditor::mouseDown(const juce::MouseEvent& e)
{
    const auto hit = pointAt(e.position);
    const Modulator* m = modulator_();
    if (!hit || !m)
        return;
    if (e.mods.isPopupMenu()) {
        if (m->points.size() > 1)
            change_([i = *hit](Modulator& edited) { edited.points.erase(edited.points.begin() + std::ptrdiff_t(i)); });
        return;
    }
    dragging_ = hit;
    dragPoints_ = m->points;
    moved_ = false;
    lastSend_ = 0.0;
    session_.client().beginGesture();
    repaint();
}

void LfoEditor::mouseDrag(const juce::MouseEvent& e)
{
    const auto m = shown();
    if (!dragging_ || !m)
        return;
    const size_t i = *dragging_;
    const double low = i > 0 ? dragPoints_[i - 1].phase + kPointGap : 0.0;
    const double high = i + 1 < dragPoints_.size() ? dragPoints_[i + 1].phase - kPointGap : 1.0 - kPointGap;
    const auto r = plot();
    LfoPoint& point = dragPoints_[i];
    point.phase = std::clamp(double((e.position.x - r.getX()) / r.getWidth()), low, std::max(low, high));
    point.value = sourceAt(*m, e.position.y, point.value);
    moved_ = true;
    if (seconds() - lastSend_ >= kSendInterval)
        sendPoints();
    repaint();
}

void LfoEditor::mouseUp(const juce::MouseEvent&)
{
    if (!dragging_)
        return;
    if (moved_)
        sendPoints();
    session_.client().endGesture();
    dragging_.reset();
    repaint();
}

void LfoEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto m = shown();
    if (!m || m->shape != LfoShape::Custom)
        return;
    if (const auto hit = pointAt(e.position))
        return change_([i = *hit](Modulator& edited) { edited.points[i].smooth = !edited.points[i].smooth; });
    if (m->points.size() >= kMaxLfoPoints)
        return;
    const auto r = plot();
    const double phase = std::clamp(double((e.position.x - r.getX()) / r.getWidth()), 0.0, 1.0 - kPointGap);
    const auto next = std::find_if(m->points.begin(), m->points.end(), [&](const LfoPoint& p) { return p.phase >= phase; });
    if ((next != m->points.end() && next->phase - phase < kPointGap) ||
        (next != m->points.begin() && phase - std::prev(next)->phase < kPointGap))
        return;
    const LfoPoint point{phase, sourceAt(*m, e.position.y, lfoAt(m->shape, phase, m->points))};
    change_([index = next - m->points.begin(), point](Modulator& edited) {
        edited.points.insert(edited.points.begin() + index, point);
    });
}

juce::String LfoEditor::getTooltip()
{
    const auto m = shown();
    if (m && m->shape == LfoShape::Custom)
        return "One LFO cycle: double-click to add a point or make one smooth or straight, drag a point to move it, "
               "right-click to remove it";
    return "One LFO cycle as it moves the value, with the playhead at the beat; pick Custom to draw your own";
}

void LfoEditor::sendPoints()
{
    lastSend_ = seconds();
    change_([points = dragPoints_](Modulator& edited) { edited.points = points; });
}

void LfoEditor::timerCallback()
{
    const Modulator* m = modulator_();
    if (isShowing() && m && m->source == ModulatorSource::Lfo)
        repaint();
}

namespace {

const char* const kEnvelopeLabels[] = {"Meter", "Punchy", "Smooth", "Breathe", "Gate", "Custom"};

// A time set by hand makes the envelope Custom, an old show's modulator included.
void setTime(Modulator& m, double Modulator::*time, double ms)
{
    m.*time = ms;
    if (m.envelope == EnvelopePreset::None)
        m.envelope = EnvelopePreset::Custom;
}

} // namespace

EnvelopeControls::EnvelopeControls(Session& session, Change change)
    : change_(std::move(change)), attack_(session, 0, kMaxEnvelopeMs, 5, " ms"),
      hold_(session, 0, kMaxEnvelopeMs, 5, " ms"), release_(session, 0, kMaxEnvelopeMs, 10, " ms")
{
    setInterceptsMouseClicks(false, true);
    for (int i = 0; i < 6; ++i)
        preset_.addItem(kEnvelopeLabels[i], i + 1);
    preset_.setTooltip("How it follows the sound. Meter: up at once, down like a level meter; Punchy: falls fast; "
                       "Smooth: eases up, falls slowly; Breathe: follows phrases; Gate: a flash on each hit; Custom: "
                       "your own times; Old: smoothed as in shows from before these presets");
    raw_.setTooltip("Raw: follows the sound unsmoothed, as in shows from before envelope presets; click for Meter");
    raw_.setColour(juce::TextButton::buttonColourId, Look::palette().fault);
    times_.setTooltip("Show or hide the attack, hold and release times");
    times_.setClickingTogglesState(true);
    const juce::String ms = " ms, 0 to " + juce::String(kMaxEnvelopeMs, 0) + ": drag sideways or double-click; "
                            "makes it Custom";
    attack_.setTooltip("A: attack, how fast it rises on a hit (to 90%), in" + ms);
    hold_.setTooltip("H: hold, how long a peak stays before it falls, in" + ms);
    release_.setTooltip("R: release, how long it takes to fall by half (Gate: to fall to 0), in" + ms);
    preset_.onChange = [this] {
        const int id = preset_.getSelectedId();
        if (id > 0)
            change_([id](Modulator& m) { setEnvelope(m, EnvelopePreset(id)); });
    };
    raw_.onClick = [this] { change_([](Modulator& m) { setEnvelope(m, EnvelopePreset::Meter); }); };
    times_.onClick = [this] {
        open_ = times_.getToggleState();
        layOut();
        repaint();
    };
    auto time = [this](double Modulator::*field) {
        return [this, field](double ms) { change_([field, ms](Modulator& m) { setTime(m, field, ms); }); };
    };
    attack_.onChange = time(&Modulator::attack);
    hold_.onChange = time(&Modulator::hold);
    release_.onChange = time(&Modulator::release);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&preset_, &raw_, &times_, &attack_, &hold_,
                                                                      &release_})
        addChildComponent(c);
    times_.setVisible(true);
    layOut();
}

void EnvelopeControls::refresh(const Modulator& m)
{
    const bool raw = m.envelope == EnvelopePreset::None && m.attack == 0.0 && m.release == 0.0;
    if (m.envelope == EnvelopePreset::None)
        preset_.setText(raw ? "Raw" : "Old", juce::dontSendNotification);
    else
        preset_.setSelectedId(int(m.envelope), juce::dontSendNotification);
    attack_.setValue(m.attack);
    hold_.setValue(m.hold);
    release_.setValue(m.release);
    if (raw == rawShown_)
        return;
    rawShown_ = raw;
    layOut();
}

void EnvelopeControls::resized()
{
    layOut();
}

void EnvelopeControls::layOut()
{
    auto area = getLocalBounds();
    times_.setBounds(area.removeFromRight(24));
    area.removeFromRight(4);
    preset_.setVisible(!open_ && !rawShown_);
    raw_.setVisible(!open_ && rawShown_);
    preset_.setBounds(area);
    raw_.setBounds(area);
    const int width = area.getWidth() / 3;
    for (NumberField* field : {&attack_, &hold_, &release_}) {
        field->setVisible(open_);
        field->setBounds(area.removeFromLeft(width).withTrimmedLeft(12));
    }
}

void EnvelopeControls::paint(juce::Graphics& g)
{
    if (!open_)
        return;
    g.setColour(Look::palette().muted);
    g.setFont(Look::smallFont());
    for (const auto& [label, field] : {std::pair{"A", &attack_}, std::pair{"H", &hold_}, std::pair{"R", &release_}})
        drawText(g, label, field->getBounds().translated(-12, 0).withWidth(10), juce::Justification::centred);
}

AnimateBox::AnimateBox(Session& session, ParamRef ref)
    : AnimateBox(session, paramInfo(session.composition(), ref).value_or(ParamInfo{ref}))
{
}

AnimateBox::AnimateBox(Session& session, const ParamInfo& info)
    : session_(session), ref_(info.ref), scale_(info.unit.scale),
      envelope_(session,
                [this](const auto& change) { send([change](Modulator& m, const ParamInfo&) { change(m); }); }),
      from_(session, info.min * scale_, info.max * scale_, std::pow(10.0, -info.unit.decimals), utf8(info.unit.suffix)),
      to_(session, info.min * scale_, info.max * scale_, std::pow(10.0, -info.unit.decimals), utf8(info.unit.suffix)),
      lfo_(session, [this] { return animationOf(session_.composition(), ref_); },
           [this](const auto& change) { send([change](Modulator& m, const ParamInfo&) { change(m); }); })
{
    for (int i = 0; i < kSourceCount; ++i)
        source_.addItem(kSourceNames[i], i + 1);
    for (size_t i = 0; i < std::size(kShapes); ++i)
        shape_.addItem(kShapeNames[i], int(i) + 1);
    for (int beats : kBeats)
        beats_.addItem(juce::String(beats) + (beats == 1 ? " beat" : " beats"), beats);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&source_, &shape_, &beats_, &stop_, &envelope_,
                                                                      &from_, &to_, &lfo_, &autoRange_, &learned_})
        addAndMakeVisible(c);
    autoRange_.setTooltip("Map the range the feature has had over the last 30 s of music, not raw 0..1, so it swings "
                          "the same at any input level");
    autoRange_.onClick = [this] {
        const bool on = autoRange_.getToggleState();
        send([on](Modulator& m, const ParamInfo&) { m.autoRange = on; });
    };
    learned_.setTooltip("The range of the feature Auto range has learned, which it maps onto from..to");
    learned_.setFont(Look::smallFont());
    learned_.setJustificationType(juce::Justification::centredRight);
    stop_.setTooltip("Stop animating");
    source_.setTooltip("What animates it: an LFO on the beat, or the kick, snare, hat, bass or the Low, Mid or High band of the audio input");
    shape_.setTooltip("The LFO's wave shape");
    beats_.setTooltip("How many beats one LFO cycle takes");
    from_.setTooltip("From: the LFO's low end, or where an audio source rests; " + rangeText(info));
    to_.setTooltip("To: the LFO's high end, or an audio source's peak; " + rangeText(info));
    stop_.onClick = [this] { session_.stopAnimating(ref_); };
    source_.onChange = [this] {
        const int i = source_.getSelectedId() - 1;
        if (i >= 0)
            session_.animate(ref_, kSources[i]);
    };
    shape_.onChange = [this] {
        const int i = shape_.getSelectedId() - 1;
        send([i](Modulator& m, const ParamInfo&) {
            if (i >= 0)
                setLfoShape(m, kShapes[i]);
        });
    };
    beats_.onChange = [this] {
        const int beats = beats_.getSelectedId();
        send([beats](Modulator& m, const ParamInfo&) { m.beats = beats; });
    };
    auto rangeEnd = [this](double Modulator::*end) {
        return [this, end](double shown) {
            send([this, end, shown](Modulator& m, const ParamInfo& info) {
                m.*end = offsetTo(info, shown / scale_, m.*end);
            });
        };
    };
    from_.onChange = rangeEnd(&Modulator::min);
    to_.onChange = rangeEnd(&Modulator::max);
    refresh();
}

void AnimateBox::send(const std::function<void(Modulator&, const ParamInfo&)>& change)
{
    const Composition& composition = session_.composition();
    const Modulator* existing = animationOf(composition, ref_);
    const auto info = paramInfo(composition, ref_);
    if (!existing || !info)
        return;
    Modulator m = *existing;
    change(m, *info);
    if (!(m == *existing))
        session_.client().edit(SetModulator{m});
}

void AnimateBox::refresh()
{
    const Composition& composition = session_.composition();
    const Modulator* m = animationOf(composition, ref_);
    const auto info = paramInfo(composition, ref_);
    if (!m || !info)
        return;
    const std::string source = animationSource(*m);
    for (int i = 0; i < kSourceCount; ++i)
        if (source == kSources[i])
            source_.setSelectedId(i + 1, juce::dontSendNotification);
    shape_.setSelectedId(0, juce::dontSendNotification);
    for (size_t i = 0; i < std::size(kShapes); ++i)
        if (m->shape == kShapes[i])
            shape_.setSelectedId(int(i) + 1, juce::dontSendNotification);
    beats_.setSelectedId(int(m->beats), juce::dontSendNotification);
    envelope_.refresh(*m);
    lfo_.repaint();
    const Ends ends = endsOf(*info, m->min, m->max);
    from_.setValue(info->fit(ends.from) * scale_);
    to_.setValue(info->fit(ends.to) * scale_);
    autoRange_.setToggleState(m->autoRange, juce::dontSendNotification);
    juce::String learned;
    if (m->autoRange) {
        const auto& ranges = session_.modulation().ranges;
        const auto range = ranges.find(m->id);
        learned = range == ranges.end() ? juce::String("learning")
                                        : "learned " + juce::String(range->second.first * 100.0, 0) + "-" +
                                              juce::String(range->second.second * 100.0, 0) + " %";
    }
    learned_.setText(learned, juce::dontSendNotification);
    const bool beat = m->source == ModulatorSource::Lfo;
    if (beat != beat_) {
        beat_ = beat;
        resized();
    }
}

int AnimateBox::preferredHeight() const
{
    const Modulator* m = animationOf(session_.composition(), ref_);
    if (!m)
        return kHeight;
    return kHeight + (m->source == ModulatorSource::Lfo ? LfoEditor::kHeight + 3 : kAudioLine);
}

void AnimateBox::resized()
{
    auto area = getLocalBounds().reduced(6, 2).withTrimmedLeft(6);
    auto line = [&] {
        auto r = area.removeFromTop(18);
        area.removeFromTop(3);
        return r;
    };
    auto first = line();
    stop_.setBounds(first.removeFromRight(20));
    first.removeFromRight(3);
    source_.setBounds(first);

    auto second = line();
    shape_.setVisible(beat_);
    beats_.setVisible(beat_);
    envelope_.setVisible(!beat_);
    envelope_.setBounds(second);
    shape_.setBounds(second.removeFromLeft(second.getWidth() / 2 - 2));
    second.removeFromLeft(4);
    beats_.setBounds(second);
    auto third = line();
    from_.setBounds(third.removeFromLeft(third.getWidth() / 2 - 8));
    third.removeFromLeft(16);
    to_.setBounds(third);
    autoRange_.setVisible(!beat_);
    learned_.setVisible(!beat_);
    if (!beat_) {
        auto fourth = line();
        autoRange_.setBounds(fourth.removeFromLeft(fourth.getWidth() / 2));
        learned_.setBounds(fourth);
    }
    lfo_.setVisible(beat_);
    lfo_.setBounds(area.removeFromTop(LfoEditor::kHeight));
}

void AnimateBox::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setColour(juce::Colour(0xff141414));
    g.fillRect(getLocalBounds().reduced(6, 0).withTrimmedLeft(6));
    g.setColour(p.accent);
    g.fillRect(getLocalBounds().reduced(6, 0).withTrimmedLeft(6).withWidth(2));
    g.setColour(p.muted);
    g.setFont(Look::smallFont());
    drawText(g, juce::String::fromUTF8("\xe2\x86\x92"), to_.getBounds().translated(-16, 0).withWidth(16),
               juce::Justification::centred);
}

namespace {

bool inMacro(const Composition& composition, const ParamRef& ref)
{
    return std::any_of(composition.macros.begin(), composition.macros.end(), [&](const Macro& macro) {
        return std::any_of(macro.targets.begin(), macro.targets.end(),
                           [&](const MacroTarget& target) { return target.param == ref; });
    });
}

bool stepped(const Composition& composition, const ParamRef& ref)
{
    const Effect* effect = ref.effect ? effectById(composition, ref.effect) : nullptr;
    return effect && std::any_of(effect->steps.begin(), effect->steps.end(),
                                 [&](const EffectStep& step) { return step.params.contains(ref.param); });
}

// The value a macro or modulator drives the parameter to, or its effect's steps move it to, while one does.
std::optional<double> drivenNow(const Session& session, const ParamRef& ref)
{
    const Composition& composition = session.composition();
    if (!animationOf(composition, ref) && !inMacro(composition, ref) && !stepped(composition, ref))
        return std::nullopt;
    return session.drivenValue(ref);
}

bool isSwitch(const ParamInfo& info)
{
    return info.options() == std::vector<std::string>{"Off", "On"};
}

} // namespace

ParamRow::ParamRow(Session& session, ParamRef ref) : session_(session), ref_(std::move(ref))
{
    refresh();
}

int ParamRow::preferredHeight() const
{
    return kHeight + (animate_ ? animate_->preferredHeight() : 0);
}

void ParamRow::refresh()
{
    const auto info = paramInfo(session_.composition(), ref_);
    if (info && measuredRange_ != std::make_pair(info->min, info->max)) {
        measuredRange_ = std::make_pair(info->min, info->max);
        valueWidth_ = kValueWidth;
        for (const double v : {info->min, info->max, info->defaultValue, info->min + (info->max - info->min) / 3.0})
            valueWidth_ = std::max(valueWidth_,
                                   juce::GlyphArrangement::getStringWidthInt(Look::font(), formatValue(*info, v)) + 2);
    }
    if (info && isSwitch(*info))
        showSwitch(*info);
    else if (info && !info->options().empty())
        showOptions(*info);
    const bool open = session_.animationOpen(ref_) && animationOf(session_.composition(), ref_);
    if (open && !animate_) {
        animate_ = std::make_unique<AnimateBox>(session_, ref_);
        addAndMakeVisible(*animate_);
        resized();
    } else if (!open && animate_) {
        animate_.reset();
    }
    if (animate_) {
        animate_->refresh();
        if (animate_->getHeight() != animate_->preferredHeight())
            resized();
    }
    repaint();
}

void ParamRow::refreshDriven()
{
    const auto driven = session_.drivenValue(ref_);
    // The learned range an open Auto range box shows moves on its own, slower than the driven value.
    if (animate_ && animate_->autoRangeButton().getToggleState())
        animate_->refresh();
    if (driven == shownDriven_)
        return;
    shownDriven_ = driven;
    if (animate_)
        animate_->refresh();
    if (const auto info = paramInfo(session_.composition(), ref_)) {
        if (options_)
            showOptions(*info);
        if (switch_)
            showSwitch(*info);
    }
    repaint();
}

void ParamRow::showOptions(const ParamInfo& info)
{
    if (!options_) {
        options_ = std::make_unique<juce::ComboBox>();
        for (size_t i = 0; i < info.options().size(); ++i)
            options_->addItem(utf8(info.options()[i]), int(i) + 1);
        options_->setTooltip(utf8(info.fullLabel()) + ": pick one; right-click to animate it or put it on a macro");
        options_->onChange = [this] {
            const auto now = paramInfo(session_.composition(), ref_);
            const int index = options_->getSelectedItemIndex();
            if (now && now->def && index >= 0)
                session_.setParam(ref_, optionValue(*now->def, index));
        };
        options_->addMouseListener(this, false);
        addAndMakeVisible(*options_);
        resized();
    }
    options_->setInterceptsMouseClicks(!session_.learning(), false);
    const int shown = optionIndex(*info.def, drivenNow(session_, ref_).value_or(info.value));
    options_->setSelectedItemIndex(shown, juce::dontSendNotification);
}

void ParamRow::showSwitch(const ParamInfo& info)
{
    if (!switch_) {
        switch_ = std::make_unique<juce::ToggleButton>();
        switch_->setTooltip(utf8(info.fullLabel()) + ": click to switch; right-click to animate it or put it on a macro");
        switch_->onClick = [this] {
            if (const auto now = paramInfo(session_.composition(), ref_); now && now->def)
                session_.setParam(ref_, optionValue(*now->def, switch_->getToggleState() ? 1 : 0));
        };
        switch_->addMouseListener(this, false);
        addAndMakeVisible(*switch_);
        resized();
    }
    switch_->setInterceptsMouseClicks(!session_.learning(), false);
    const int shown = optionIndex(*info.def, drivenNow(session_, ref_).value_or(info.value));
    switch_->setToggleState(shown == 1, juce::dontSendNotification);
}

void refreshDrivenRows(juce::Component& root)
{
    for (juce::Component* child : root.getChildren()) {
        if (auto* row = dynamic_cast<ParamRow*>(child))
            row->refreshDriven();
        else if (auto* swatch = dynamic_cast<SwatchRow*>(child))
            swatch->refreshDriven();
        else
            refreshDrivenRows(*child);
    }
}

void ParamRow::resized()
{
    if (options_)
        options_->setBounds(barArea().getX(), 2, valueArea().getRight() - barArea().getX(), kHeight - 4);
    if (switch_)
        switch_->setBounds(barArea().getX(), 2, valueArea().getRight() - barArea().getX(), kHeight - 4);
    if (animate_)
        animate_->setBounds(0, kHeight, getWidth(), animate_->preferredHeight());
}

juce::Rectangle<int> ParamRow::labelArea() const
{
    return {8, 0, kLabelWidth, kHeight};
}

juce::Rectangle<int> ParamRow::valueArea() const
{
    const int width = std::max(kValueWidth, valueWidth_);
    return {getWidth() - 8 - width, 0, width, kHeight};
}

juce::Rectangle<int> ParamRow::barArea() const
{
    const int left = labelArea().getRight() + 4;
    return {left, 4, valueArea().getX() - 4 - left, kHeight - 8};
}

juce::Rectangle<int> ParamRow::tagArea() const
{
    return chevronBox(labelArea().withLeft(labelArea().getRight() - Look::metrics().chevronBox));
}

void ParamRow::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const Composition& composition = session_.composition();
    const auto info = paramInfo(composition, ref_);
    if (!info)
        return;
    const bool animated = animationOf(composition, ref_) != nullptr;
    const bool macro = inMacro(composition, ref_);
    if (session_.focusedParam() == ref_) {
        g.setColour(p.liveRow);
        g.fillRect(getLocalBounds().withHeight(kHeight));
    }

    g.setFont(Look::font());
    g.setColour(p.muted);
    auto label = labelArea();
    if (animated) {
        g.setColour(p.live);
        auto tag = label.removeFromRight(tagArea().getWidth() + 8);
        drawText(g, "~", tag.removeFromLeft(8), juce::Justification::centredRight);
        drawChevron(g, tagArea(), session_.animationOpen(ref_), p.live);
        g.setColour(p.muted);
    }
    drawText(g, utf8(info->label), label, juce::Justification::centredLeft, true);
    if (juce::Component* control = options_ ? static_cast<juce::Component*>(options_.get()) : switch_.get()) {
        if (session_.learning() || macro) {
            g.setColour(session_.learning() ? p.accent : p.live);
            g.drawRect(control->getBounds(), 1);
        }
        return;
    }

    const double base = dragValue_.value_or(info->value);
    const auto driven = drivenNow(session_, ref_);
    const auto bar = barArea();
    const Modulator* animation = animationOf(composition, ref_);
    if (animation) {
        ParamInfo moved = *info;
        moved.value = base;
        const auto [min, max] = dragEnd_ ? dragOffsets_ : std::pair(animation->min, animation->max);
        drawRangeBar(g, bar, moved, endsOf(moved, min, max), dragValue_ ? std::nullopt : driven);
    } else if (driven && !dragValue_) {
        drawBar(g, bar, float(info->fraction(*driven)), p.accent);
        g.setColour(p.white);
        g.fillRect(bar.getX() + juce::roundToInt(float(bar.getWidth()) * float(std::clamp(info->fraction(base), 0.0, 1.0))) - 1,
                   bar.getY(), 2, bar.getHeight());
    } else {
        drawBar(g, bar, float(info->fraction(base)), p.meter);
    }
    if (session_.learning()) {
        g.setColour(p.accent);
        g.drawRect(bar, 1);
    } else if (macro) {
        g.setColour(p.live);
        g.drawRect(bar, 1);
    }
    g.setColour(p.text);
    const bool mixed = !dragValue_ && session_.mixedParam(ref_);
    drawText(g, mixed ? juce::String("mixed") : formatValue(*info, driven && !dragValue_ ? *driven : base), valueArea(),
               juce::Justification::centredRight);
}

std::optional<int> ParamRow::handleX(int end) const
{
    const Composition& composition = session_.composition();
    const Modulator* animation = animationOf(composition, ref_);
    const auto info = paramInfo(composition, ref_);
    if (!animation || !info)
        return std::nullopt;
    const Ends ends = endsOf(*info, animation->min, animation->max);
    return juce::roundToInt(xOf(barArea(), *info, end ? ends.to : ends.from));
}

void ParamRow::setEndFromX(int x)
{
    const auto info = paramInfo(session_.composition(), ref_);
    if (!info)
        return;
    double& offset = *dragEnd_ ? dragOffsets_.second : dragOffsets_.first;
    offset = offsetTo(*info, valueAtX(barArea(), *info, x), offset);
    if (seconds() - lastSend_ >= kSendInterval)
        sendEnds();
    repaint();
}

void ParamRow::sendEnds()
{
    lastSend_ = seconds();
    const Modulator* animation = animationOf(session_.composition(), ref_);
    if (!animation)
        return;
    Modulator m = *animation;
    std::tie(m.min, m.max) = dragOffsets_;
    if (!(m == *animation))
        session_.client().edit(SetModulator{m});
}

void ParamRow::setFromX(int x)
{
    const auto info = paramInfo(session_.composition(), ref_);
    if (!info)
        return;
    dragValue_ = valueAtX(barArea(), *info, x);
    if (seconds() - lastSend_ >= kSendInterval) {
        session_.setParam(ref_, *dragValue_);
        lastSend_ = seconds();
    }
    repaint();
}

void ParamRow::mouseDown(const juce::MouseEvent& e)
{
    if (e.eventComponent == options_.get() || e.eventComponent == switch_.get()) {
        if (e.mods.isPopupMenu())
            showMenu();
        return;
    }
    if (e.mods.isPopupMenu())
        return showMenu();
    if (e.y >= kHeight)
        return;
    if (tagArea().contains(e.getPosition()) && animationOf(session_.composition(), ref_))
        return session_.setAnimationOpen(ref_, !session_.animationOpen(ref_));
    if (!barArea().expanded(0, 4).contains(e.getPosition()))
        return;
    if (session_.learning())
        return session_.assignToMacro(session_.selectedMacro(), ref_);
    session_.client().beginGesture();
    lastSend_ = 0.0;
    const Composition& composition = session_.composition();
    const Modulator* animation = animationOf(composition, ref_);
    const auto info = paramInfo(composition, ref_);
    if (animation && info) {
        dragEnd_ = rangeHandleAt(barArea(), *info, endsOf(*info, animation->min, animation->max), e.x);
        dragOffsets_ = {animation->min, animation->max};
    }
    if (dragEnd_)
        return setEndFromX(e.x);
    setFromX(e.x);
}

void ParamRow::mouseDrag(const juce::MouseEvent& e)
{
    if (e.eventComponent == options_.get() || e.eventComponent == switch_.get())
        return;
    if (dragEnd_)
        setEndFromX(e.x);
    else if (dragValue_)
        setFromX(e.x);
}

void ParamRow::mouseUp(const juce::MouseEvent&)
{
    if (dragEnd_) {
        sendEnds();
        session_.client().endGesture();
        dragEnd_.reset();
        repaint();
        return;
    }
    if (!dragValue_)
        return;
    session_.setParam(ref_, *dragValue_);
    session_.client().endGesture();
    dragValue_.reset();
    repaint();
}

juce::String ParamRow::tipAt(juce::Point<int> point)
{
    const Composition& composition = session_.composition();
    const auto info = paramInfo(composition, ref_);
    if (!info || point.y >= kHeight)
        return {};
    const Modulator* animation = animationOf(composition, ref_);
    if (tagArea().contains(point) && animation)
        return "Show or hide its animation settings";
    if (labelArea().contains(point))
        return utf8(info->fullLabel()) + ": right-click to animate it or put it on a macro";
    if (valueArea().contains(point))
        return utf8(info->label) + ", " + rangeText(*info) + ": double-click to type";
    if (!barArea().expanded(0, 4).contains(point))
        return {};
    if (session_.learning())
        return "Click to add it to the macro being assigned";
    if (animation)
        if (auto end = rangeHandleAt(barArea(), *info, endsOf(*info, animation->min, animation->max), point.x))
            return juce::String(*end ? "To" : "From") + ": drag this end of the animation's range";
    return utf8(info->label) + ", " + rangeText(*info) + ": drag to set, right-click to animate or put on a macro";
}

void ParamRow::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto info = paramInfo(session_.composition(), ref_);
    if (!info || options_ || switch_ || !valueArea().contains(e.getPosition()))
        return;
    const double scale = info->unit.scale;
    editInline(editor_, *this, valueArea().expanded(4, 0), juce::String(info->value * scale, info->unit.decimals + 1),
               [this, scale, info = *info](const juce::String& text) {
                   const double typed = text.retainCharacters("-0123456789.").getDoubleValue();
                   session_.setParam(ref_, info.fit(typed / scale));
               });
}

double valueAtX(juce::Rectangle<int> bar, const ParamInfo& info, int x)
{
    return info.at(std::clamp(double(x - bar.getX()) / std::max(1, bar.getWidth()), 0.0, 1.0));
}

void drawRangeBar(juce::Graphics& g, juce::Rectangle<int> bar, const ParamInfo& info, Ends ends,
                  std::optional<double> live)
{
    const Palette& p = Look::palette();
    g.setColour(p.field);
    g.fillRect(bar);
    const double span = info.max - info.min;
    const double lo = std::min(ends.from, ends.to), hi = std::max(ends.from, ends.to);
    g.setColour(p.accent.withAlpha(0.35f));
    const int turns = info.wraps && span > 0.0 ? 2 : 0;
    for (int k = -turns; k <= turns; ++k) {
        const double a = std::max(lo + k * span, info.min), b = std::min(hi + k * span, info.max);
        if (b > a)
            g.fillRect(juce::Rectangle<float>(xOf(bar, info, a), float(bar.getY()), xOf(bar, info, b) - xOf(bar, info, a),
                                              float(bar.getHeight())));
    }
    if (live) {
        g.setColour(p.white);
        g.fillRect(juce::Rectangle<float>(xOf(bar, info, *live) - 1.0f, float(bar.getY()), 2.0f, float(bar.getHeight())));
    }
    const juce::Rectangle<float> handle(3.0f, float(bar.getHeight() + 6));
    g.setColour(p.text);
    g.fillRect(handle.withCentre({xOf(bar, info, ends.from), float(bar.getCentreY())}));
    g.setColour(p.accent);
    g.fillRect(handle.withCentre({xOf(bar, info, ends.to), float(bar.getCentreY())}));
}

std::optional<int> rangeHandleAt(juce::Rectangle<int> bar, const ParamInfo& info, Ends ends, int x)
{
    const float from = std::abs(xOf(bar, info, ends.from) - float(x)), to = std::abs(xOf(bar, info, ends.to) - float(x));
    if (std::min(from, to) > 5.0f)
        return std::nullopt;
    return to <= from ? 1 : 0;
}

void ParamRow::showMenu()
{
    const Composition& composition = session_.composition();
    const auto info = paramInfo(composition, ref_);
    if (!info)
        return;
    const auto slots = macroSlots(composition);
    juce::PopupMenu add;
    for (int i = 0; i < kMacroSlots; ++i) {
        const Macro* macro = slots[size_t(i)];
        const bool has = macro && std::any_of(macro->targets.begin(), macro->targets.end(),
                                              [&](const MacroTarget& t) { return t.param == ref_; });
        add.addItem(100 + i, macro ? utf8(macro->name) : "Macro " + juce::String(i + 1), !has, has);
    }
    juce::PopupMenu menu;
    menu.addSubMenu("Add to macro", add);
    std::vector<uint64_t> containing;
    for (const Macro& macro : composition.macros)
        if (std::any_of(macro.targets.begin(), macro.targets.end(), [&](const MacroTarget& t) { return t.param == ref_; })) {
            menu.addItem(200 + int(containing.size()), "Remove from " + utf8(macro.name));
            containing.push_back(macro.id);
        }
    const Modulator* animation = animationOf(composition, ref_);
    juce::PopupMenu animate;
    for (int i = 0; i < kSourceCount; ++i)
        animate.addItem(300 + i, kSourceNames[i], true, animation && animationSource(*animation) == kSources[i]);
    animate.addSeparator();
    animate.addItem(400, session_.animationOpen(ref_) ? "Hide animation settings" : "Show animation settings",
                    animation != nullptr);
    animate.addItem(401, "None", animation != nullptr);
    menu.addSubMenu("Animate", animate);
    menu.addSeparator();
    menu.addItem(500, "Reset to default");

    Session& session = session_;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                       [&session, ref = ref_, containing, def = info->defaultValue](int result) {
                           if (result >= 100 && result < 100 + kMacroSlots)
                               session.assignToMacro(result - 100, ref);
                           else if (result >= 200 && result < 200 + int(containing.size()))
                               session.removeFromMacro(containing[size_t(result - 200)], ref);
                           else if (result >= 300 && result < 300 + kSourceCount)
                               session.animate(ref, kSources[result - 300]);
                           else if (result == 400)
                               session.setAnimationOpen(ref, !session.animationOpen(ref));
                           else if (result == 401)
                               session.stopAnimating(ref);
                           else if (result == 500) {
                               session.client().beginGesture();
                               session.stopAnimating(ref);
                               session.setParam(ref, def);
                               session.client().endGesture();
                           }
                       });
}

juce::ScaledImage dragImage(juce::Component& component, juce::Rectangle<int> area)
{
    constexpr float kScale = 2.0f;
    juce::Image image = component.createComponentSnapshot(area, true, kScale).convertedToFormat(juce::Image::ARGB);
    image.multiplyAllAlphas(0.6f);
    return {image, kScale};
}

void dropEffectCard(Session& session, const juce::String& description, const ChainOwner& owner,
                    std::optional<size_t> gap)
{
    const uint64_t effect = uint64_t(description.fromFirstOccurrenceOf(":", false, false).getLargeIntValue());
    const bool copy = juce::ModifierKeys::currentModifiers.isAltDown();
    if (auto edit = moveEffectTo(session.composition(), effect, owner, gap, copy))
        session.client().edit(*edit);
}

void startDrag(juce::Component& component, const juce::String& description, juce::Rectangle<int> area,
               const juce::MouseEvent& e)
{
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(&component)) {
        const juce::Point<int> offset = area.getPosition() - e.getMouseDownPosition();
        container->startDragging(description, &component, dragImage(component, area), false, &offset);
    }
}

bool isEffectDrag(const juce::String& description)
{
    return description.startsWith("effect:") || description.startsWith("preset:") || description.startsWith("chain:");
}

std::vector<Effect> draggedEffects(Session& session, const juce::String& description)
{
    const juce::String rest = description.fromFirstOccurrenceOf(":", false, false);
    if (description.startsWith("effect:"))
        return {newEffect(session.client(), rest.toStdString())};
    if (description.startsWith("chain:")) {
        const Message chain = Message::parse(rest.toStdString(), nullptr, false);
        if (!chain.is_object())
            return {};
        return chainEffects({{"chain", chain.value("effects", Message::array())}}, "chain");
    }
    const auto effect = draggedEffect(description);
    return effect ? std::vector<Effect>{*effect} : std::vector<Effect>{};
}

std::optional<Effect> draggedEffect(const juce::String& description)
{
    const juce::String rest = description.fromFirstOccurrenceOf(":", false, false);
    if (description.startsWith("effect:"))
        return Effect{0, rest.toStdString(), {}};
    if (!description.startsWith("preset:"))
        return std::nullopt;
    const Message preset = Message::parse(rest.toStdString(), nullptr, false);
    if (!preset.is_object() || !preset.contains("effectType"))
        return std::nullopt;
    Effect effect{0, preset.value("effectType", ""), {}, false, preset.value("name", "")};
    const Message params = preset.value("params", Message::object());
    for (const auto& [name, value] : params.items())
        if (value.is_number())
            effect.params[name] = value.get<double>();
    return effect;
}

juce::String presetDrag(const PresetEntry& preset)
{
    const Message message{{"effectType", preset.effectType}, {"name", preset.name}, {"params", preset.params}};
    return "preset:" + utf8(message.dump());
}

bool dropOnSelection(Session& session, const juce::String& description, Session::ClipCell cell,
                     std::optional<size_t> gap)
{
    const std::vector<Session::ClipCell> cells = session.clipTargets(cell);
    if (cells.size() < 2 || description.startsWith("chain:"))
        return false;
    const Composition& composition = session.composition();
    const juce::String rest = description.fromFirstOccurrenceOf(":", false, false);
    std::vector<Edit> edits;
    const std::vector<Effect> dragged = draggedEffects(session, description);
    Effect effect = dragged.empty() ? Effect{} : dragged.front();
    if (description.startsWith("fxcard:")) {
        const uint64_t id = uint64_t(rest.getLargeIntValue());
        const Effect* card = effectById(composition, id);
        if (!card)
            return true;
        effect = Effect{0, card->type, card->params, card->bypass};
        const bool copy = juce::ModifierKeys::currentModifiers.isAltDown();
        if (auto move = moveEffectTo(composition, id, {cell.layer, cell.column}, gap, copy))
            edits.push_back(*move);
    } else {
        edits.push_back(AddEffect{cell.layer, gap, effect, cell.column});
    }
    for (size_t i = 1; i < cells.size(); ++i)
        edits.push_back(AddEffect{cells[i].layer, std::nullopt, effect, cells[i].column});
    session.edit(std::move(edits));
    session.rememberClipEdit(Session::addingEffect(effect));
    return true;
}

} // namespace azl::ui
