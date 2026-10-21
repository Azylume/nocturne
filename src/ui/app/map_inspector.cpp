#include "ui/app/map_view.hpp"

#include "document/mapping.hpp"
#include "ui/app/look.hpp"

#include <cmath>

namespace azl::ui {

namespace {

juce::String operator""_u(const char* text, size_t)
{
    return juce::String::fromUTF8(text);
}

constexpr int kRow = 22;
constexpr int kHeader = 20;
constexpr int kLabelWidth = 84;
constexpr double kPi = 3.14159265358979323846;

} // namespace

MapInspector::MapInspector(Session& session, MapView& map) : session_(session), map_(map) {}

std::string MapInspector::structure() const
{
    const MapState& state = map_.state();
    const Composition& composition = session_.composition();
    std::string key = std::to_string(state.screen) + "/" + std::to_string(state.item) + (state.mask ? "m" : "s") + "/" +
                      std::to_string(composition.guides.size()) + "/" +
                      std::to_string(session_.client().displays().size()) + "/" +
                      std::to_string(composition.layers.size()) + "/" + std::to_string(state.point) + "/" +
                      std::to_string(map_.shape().size()) + (state.locked ? "/locked" : "");
    if (const Slice* s = map_.slice())
        key += "/" + std::to_string(warpAnchors(*s)[0]) + "x" + std::to_string(warpAnchors(*s)[1]) +
               (isCornerWarp(s->warp) ? "c" : "");
    if (const Mask* m = map_.mask())
        key += "/" + std::to_string(m->points.size());
    if (const Screen* s = map_.screen())
        key += "/" + s->display;
    for (const OutputDisplay& d : session_.client().displays())
        key += d.mirrorOf.empty() ? "" : "/m" + d.id;
    return key;
}

void MapInspector::refresh()
{
    if (structure() != structure_) {
        triggerAsyncUpdate();
        return;
    }
    for (const auto& update : updaters_)
        update();
    repaint();
}

void MapInspector::rebuild()
{
    structure_ = structure();
    removeAllChildren();
    rows_.clear();
    updaters_.clear();
    owned_.clear();
    if (const Screen* s = map_.screen()) {
        addScreenRows(*s);
        addGuideRows();
        if (const Slice* slice = map_.slice())
            addSliceRows(*slice);
        if (const Mask* mask = map_.mask())
            addMaskRows(*mask);
    } else {
        header("No screen: add one with + Screen");
    }
    if (map_.state().locked)
        for (const auto& part : owned_)
            if (auto* button = dynamic_cast<juce::Button*>(part.get());
                !button || (button->getButtonText() != "Test card" && button->getButtonText() != "Corner target" &&
                            button->getButtonText() != "Identify"))
                part->setEnabled(false);
    resized();
    for (const auto& update : updaters_)
        update();
    repaint();
}

void MapInspector::header(const juce::String& text)
{
    rows_.push_back({text, {}, true});
}

void MapInspector::row(const juce::String& label, std::vector<juce::Component*> parts)
{
    for (juce::Component* part : parts)
        addAndMakeVisible(part);
    rows_.push_back({label, std::move(parts), false});
}

NumberField* MapInspector::number(double min, double max, double step, std::function<double()> get,
                                  std::function<void(double)> set, const juce::String& suffix)
{
    auto field = std::make_unique<NumberField>(session_, min, max, step, suffix);
    NumberField* raw = field.get();
    raw->wheel = true;
    raw->onChange = std::move(set);
    updaters_.push_back([raw, get = std::move(get)] { raw->setValue(get()); });
    owned_.push_back(std::move(field));
    return raw;
}

NumberField* MapInspector::relative(double neutral, double min, double max, double step,
                                    std::function<Points(const Points&, double)> apply, const juce::String& suffix)
{
    auto field = std::make_unique<NumberField>(session_, min, max, step, suffix);
    NumberField* raw = field.get();
    raw->wheel = true;
    raw->setValue(neutral);
    raw->setTooltip("Relative: drag, scroll or type; it goes back after each change");
    auto base = std::make_shared<std::optional<Points>>();
    raw->onChange = [this, base, apply = std::move(apply)](double value) {
        if (!*base)
            *base = map_.shape();
        if (!(*base)->empty())
            map_.sendShape(apply(**base, value));
    };
    raw->onEnd = [raw, base, neutral] {
        base->reset();
        juce::MessageManager::callAsync([field = juce::Component::SafePointer<NumberField>(raw), neutral] {
            if (field)
                field->setValue(neutral);
        });
    };
    owned_.push_back(std::move(field));
    return raw;
}

juce::TextButton* MapInspector::action(const juce::String& text, std::function<void()> click, std::function<bool()> on)
{
    auto button = std::make_unique<juce::TextButton>(text);
    juce::TextButton* raw = button.get();
    raw->onClick = std::move(click);
    if (on)
        updaters_.push_back([raw, on = std::move(on)] { raw->setToggleState(on(), juce::dontSendNotification); });
    owned_.push_back(std::move(button));
    return raw;
}

juce::ComboBox* MapInspector::choice(std::vector<std::pair<juce::String, std::string>> items,
                                     std::function<std::string()> get, std::function<void(const std::string&)> set)
{
    auto box = std::make_unique<juce::ComboBox>();
    juce::ComboBox* raw = box.get();
    for (size_t i = 0; i < items.size(); ++i)
        raw->addItem(items[i].first, int(i) + 1);
    raw->onChange = [raw, items, set = std::move(set)] {
        const int i = raw->getSelectedId() - 1;
        if (i >= 0 && i < int(items.size()))
            set(items[size_t(i)].second);
    };
    updaters_.push_back([raw, items, get = std::move(get)] {
        const std::string value = get();
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].second == value)
                raw->setSelectedId(int(i) + 1, juce::dontSendNotification);
    });
    owned_.push_back(std::move(box));
    return raw;
}

void MapInspector::addNameRow(const std::string& kind, uint64_t id, const std::string& name)
{
    juce::TextButton* button = action(utf8(name), {});
    button->setTooltip("Click to rename");
    button->onClick = [this, button, kind, id, name] {
        editInline(renaming_, *this, button->getBounds(), utf8(name), [this, kind, id](const juce::String& text) {
            session_.client().edit(Rename{kind, id, text.toStdString()});
        });
    };
    row("Name", {button});
}

void MapInspector::addScreenRows(const Screen& screen)
{
    const uint64_t id = screen.id;
    header("Screen \xc2\xb7 "_u + utf8(screen.name));
    addNameRow("screen", id, screen.name);
    std::vector<std::pair<juce::String, std::string>> outputs{{"No output", ""}};
    bool known = screen.display.empty();
    for (const OutputDisplay& d : session_.client().displays()) {
        outputs.push_back({utf8(d.name.empty() ? d.id : d.name) + " " + juce::String(d.width) + "x" +
                               juce::String(d.height) + (d.mirrorOf.empty() ? "" : " (mirrored)"),
                           d.id});
        known = known || d.id == screen.display;
    }
    if (!known)
        outputs.push_back({utf8(screen.display) + " (not attached)", screen.display});
    row("Output", {choice(
                      outputs,
                      [this] {
                          const Screen* s = map_.screen();
                          return s ? s->display : std::string();
                      },
                      [this, id](const std::string& display) {
                          session_.client().edit(SetScreenDisplay{id, display});
                      })});
    row("Mirroring", {action("Turn off", [this] { session_.client().setUnmirror(!session_.client().unmirror()); },
                             [this] { return session_.client().unmirror(); })});
    button("Turn off")->setTooltip("When macOS mirrors displays that carry different screens, the engine turns the "
                                   "mirroring off (kept on the engine machine)");
    row("Show", {action("Test card", [this, id] { map_.toggleTestCard(id); },
                        [this] {
                            const Screen* s = map_.screen();
                            return s && s->testCard;
                        }),
                 action("Corner target",
                        [this] {
                            map_.state().targets = !map_.state().targets;
                            map_.changed();
                        },
                        [this] { return map_.state().targets; }),
                 action("Identify", [this, display = screen.display] { session_.client().identify(display); })});
    button("Identify")->setTooltip("Its name and output large on this output for 3 s");
    button("Test card")->setTooltip("The composition test card through this screen's slices on its output");
    button("Corner target")->setTooltip("A crosshair across the output through the selected corner or point");
    row("", {action("Delete screen", [this, id] { session_.client().edit(RemoveScreen{id}); })});
}

void MapInspector::addGuideRows()
{
    const auto& guides = session_.composition().guides;
    if (guides.empty())
        return;
    header("Guides \xc2\xb7 composition %, through every slice"_u);
    for (size_t i = 0; i < guides.size(); ++i) {
        auto get = [this, i] {
            const auto& g = session_.composition().guides;
            return i < g.size() ? g[i].position * 100.0 : 0.0;
        };
        auto set = [this, i](double value) {
            std::vector<Guide> g = session_.composition().guides;
            if (i < g.size()) {
                g[i].position = value / 100.0;
                session_.client().edit(SetGuides{g});
            }
        };
        auto remove = [this, i] {
            std::vector<Guide> g = session_.composition().guides;
            if (i < g.size()) {
                g.erase(g.begin() + long(i));
                session_.client().edit(SetGuides{g});
            }
        };
        juce::TextButton* x = action(juce::String::fromUTF8("\xc3\x97"), remove);
        row(guides[i].vertical ? "Vertical" : "Horizontal", {number(0, 100, 0.1, get, set, "%"), x});
    }
}

void MapInspector::addTransformRows()
{
    const Screen* s = map_.screen();
    if (!s)
        return;
    const auto [w, h] = map_.pixelSize(*s);
    const double aspect = w / h;
    row("Move X \xc2\xb7 Y"_u,
        {relative(0, -10000, 10000, 1, [w](const Points& b, double v) { return transformed(b, v / w, 0.0); }, " px"),
         relative(0, -10000, 10000, 1, [h](const Points& b, double v) { return transformed(b, 0.0, v / h); }, " px")});
    row("Scale \xc2\xb7 Rotate"_u,
        {relative(100, 5, 1000, 1, [](const Points& b, double v) { return transformed(b, 0, 0, v / 100.0); }, "%"),
         relative(0, -360, 360, 1,
                  [aspect](const Points& b, double v) { return transformed(b, 0, 0, 1, v * kPi / 180.0, aspect); },
                  juce::String::fromUTF8("\xc2\xb0"))});
}

void MapInspector::addSliceRows(const Slice& slice)
{
    const Screen* screen = map_.screen();
    const uint64_t id = slice.id;
    const auto [w, h] = map_.pixelSize(*screen);
    header("Slice \xc2\xb7 "_u + utf8(slice.name));
    addNameRow("slice", id, slice.name);

    std::vector<std::pair<juce::String, std::string>> inputs{{"Composition", "0"}};
    bool known = slice.input == 0;
    forEachLayer(session_.composition().layers, [&](const Layer& layer) {
        if (layer.group || layer.id == slice.input) {
            inputs.push_back({(layer.group ? "Group: " : "Layer: ") + utf8(layer.name), std::to_string(layer.id)});
            known = known || layer.id == slice.input;
        }
    });
    if (!known)
        inputs.push_back({"Layer " + juce::String(int64_t(slice.input)), std::to_string(slice.input)});
    row("Input", {choice(
                     inputs,
                     [this] {
                         const Slice* s = map_.slice();
                         return std::to_string(s ? s->input : 0);
                     },
                     [this, id](const std::string& input) {
                         session_.client().edit(SetSliceInput{id, std::stoull(input), std::nullopt});
                     })});
    auto rectField = [this, id](int k) {
        return number(
            0, 100, 0.1,
            [this, k] {
                const Slice* s = map_.slice();
                return s ? s->inputRect[size_t(k)] * 100.0 : 0.0;
            },
            [this, id, k](double v) {
                if (const Slice* s = map_.slice()) {
                    auto r = s->inputRect;
                    r[size_t(k)] = v / 100.0;
                    session_.client().edit(SetSliceInput{id, std::nullopt, r});
                }
            },
            "%");
    };
    row("Input X \xc2\xb7 Y"_u, {rectField(0), rectField(1)});
    row("Input W \xc2\xb7 H"_u, {rectField(2), rectField(3)});
    addTransformRows();

    addWarpRows(slice);
    if (!warpAnchors(slice)[0] && !isCornerWarp(slice.warp)) {
        const char* const corners[] = {"TL", "TR", "BR", "BL"};
        for (int k = 0; k < 4; ++k) {
            auto field = [this, k, w = w, h = h](int axis) {
                const double size = axis == 0 ? w : h;
                return number(
                    -size, 2 * size, 1,
                    [this, k, axis, size] {
                        const Points p = map_.shape();
                        return p.size() == 4 ? p[size_t(k)][size_t(axis)] * size : 0.0;
                    },
                    [this, k, axis, size](double v) {
                        Points p = map_.shape();
                        if (p.size() != 4)
                            return;
                        p[size_t(k)][size_t(axis)] = v / size;
                        map_.sendShape(p);
                    });
            };
            row(corners[k], {field(0), field(1)});
        }
    }

    header("Soft edge");
    auto soft = [this, id](double SoftEdge::*part, double scale, double min, double max, double step) {
        return number(
            min, max, step,
            [this, part, scale] {
                const Slice* s = map_.slice();
                return s ? s->softEdge.*part * scale : 0.0;
            },
            [this, id, part, scale](double v) {
                if (const Slice* s = map_.slice()) {
                    SoftEdge e = s->softEdge;
                    e.*part = v / scale;
                    session_.client().edit(SetSoftEdges{{SliceSoftEdge{id, e}}, 0});
                }
            },
            scale == 100.0 ? "%" : "");
    };
    row("L \xc2\xb7 R %"_u, {soft(&SoftEdge::left, 100, 0, 100, 0.5), soft(&SoftEdge::right, 100, 0, 100, 0.5)});
    row("T \xc2\xb7 B %"_u, {soft(&SoftEdge::top, 100, 0, 100, 0.5), soft(&SoftEdge::bottom, 100, 0, 100, 0.5)});
    juce::TextButton* autoButton = action("Auto from overlap", [this] { map_.autoSoftEdge(); });
    autoButton->setTooltip("Fade this slice and every slice of the same input across their overlapping input areas");
    row("Power", {soft(&SoftEdge::power, 1, 0.1, 10, 0.1), autoButton});
    row("", {action("Reset corners",
                    [this] {
                        const Slice* s = map_.slice();
                        if (!s)
                            return;
                        const auto [nx, ny] = warpAnchors(*s);
                        if (isCornerWarp(s->warp))
                            return map_.sendShape(sampledCornerPoints(Slice{}));
                        map_.sendShape(nx ? sampledAnchors(Slice{}, nx, ny) : Points{{0, 0}, {1, 0}, {1, 1}, {0, 1}});
                    }),
             action("+ Mask on slice", [this] { map_.addMask(true); })});
    button("+ Mask on slice")->setTooltip("A mask in this slice's input, following its warp");
    row("", {action("Duplicate",
                    [this] {
                        const Screen* s = map_.screen();
                        const Slice* sl = map_.slice();
                        if (!s || !sl)
                            return;
                        Slice copy = withShape(*sl, transformed(map_.shape(), 0.04, 0.04));
                        copy.id = 0;
                        copy.name = sl->name + " copy";
                        for (auto& m : copy.masks)
                            m.id = 0;
                        session_.client().edit(AddSlice{s->id, std::nullopt, copy});
                    }),
             action("Delete", [this] { map_.removeSelected(); })});
}

void MapInspector::addPointStepper()
{
    const size_t n = map_.shape().size();
    if (n == 0)
        return;
    auto step = [this, n](int by) {
        MapState& state = map_.state();
        const int at = state.point < 0 ? (by > 0 ? -1 : 0) : state.point;
        state.point = int(((at + by) % int(n) + int(n)) % int(n));
        map_.changed();
    };
    const int index = map_.state().point;
    juce::TextButton* label = action(index < 0 ? juce::String("none") : juce::String(index + 1) + " of " + juce::String(int(n)),
                                     [] {});
    label->setEnabled(false);
    row("Point", {action("\xe2\x97\x80"_u, [step] { step(-1); }), label, action("\xe2\x96\xb6"_u, [step] { step(1); })});
}

void MapInspector::addWarpRows(const Slice& slice)
{
    const auto [nx, ny] = warpAnchors(slice);
    std::vector<std::pair<juce::String, std::string>> modes{{"Off \xc2\xb7 corner pin"_u, "0"},
                                                            {"Bezier \xc2\xb7 corners"_u, "1"},
                                                            {"Grid 3 \xc3\x97 3"_u, "3"},
                                                            {"Grid 4 \xc3\x97 4"_u, "4"},
                                                            {"Grid 5 \xc3\x97 5"_u, "5"}};
    const bool corners = isCornerWarp(slice.warp);
    const bool custom = nx && (nx != ny || nx < 3 || nx > 5);
    if (custom)
        modes.push_back({"Custom " + juce::String(nx) + "\xc3\x97"_u + juce::String(ny), "custom"});
    row("Warp", {choice(
                    modes,
                    [this] {
                        const Slice* s = map_.slice();
                        if (!s)
                            return std::string("0");
                        if (isCornerWarp(s->warp))
                            return std::string("1");
                        const auto [x, y] = warpAnchors(*s);
                        return x == y && x >= 3 && x <= 5 ? std::to_string(x) : x ? "custom" : "0";
                    },
                    [this](const std::string& mode) {
                        if (mode != "custom")
                            map_.setWarp(std::stoi(mode));
                    })});
    if (!nx && !corners)
        return;
    juce::TextButton* flat = action("Flatten to corners", [this] { map_.flattenWarp(); });
    flat->setTooltip("Keep the four corners, straighten everything between them");
    row("", {flat});
    addPointStepper();
    const Screen* screen = map_.screen();
    const auto [w, h] = map_.pixelSize(*screen);
    const int index = map_.state().point;
    if (index < 0)
        return;
    auto coordinate = [this, index, w = w, h = h](int axis) {
        const double size = axis == 0 ? w : h;
        return number(
            -size, 2 * size, 1,
            [this, index, axis, size] {
                const Points p = map_.shape();
                return index < int(p.size()) ? p[size_t(index)][size_t(axis)] * size : 0.0;
            },
            [this, index, axis, size](double v) {
                const Points p = map_.shape();
                if (index >= int(p.size()))
                    return;
                Point d{0.0, 0.0};
                d[size_t(axis)] = v / size - p[size_t(index)][size_t(axis)];
                map_.sendShape(map_.movedPoint(p, index, d));
            });
    };
    row("Point X \xc2\xb7 Y"_u, {coordinate(0), coordinate(1)});
}

void MapInspector::addMaskRows(const Mask& mask)
{
    const uint64_t id = mask.id;
    const bool onSlice = map_.maskOwner() != nullptr;
    const auto [w, h] = map_.pixelSize(*map_.screen());
    header((onSlice ? "Slice mask \xc2\xb7 "_u : "Mask \xc2\xb7 "_u) + utf8(mask.name));
    addNameRow("mask", id, mask.name);
    row("Mode", {action(
                    "Inverted: shows only inside",
                    [this, id] {
                        const Mask* m = map_.mask();
                        session_.client().edit(SetMask{id, std::nullopt, !(m && m->invert), std::nullopt});
                    },
                    [this] {
                        const Mask* m = map_.mask();
                        return m && m->invert;
                    })});
    addTransformRows();
    if (onSlice)
        header("Points \xc2\xb7 % of the slice's input"_u);
    for (size_t k = 0; k < mask.points.size(); ++k) {
        auto field = [this, k, id, onSlice, w = w, h = h](int axis) {
            const double size = onSlice ? 100.0 : axis == 0 ? w : h;
            return number(
                onSlice ? -100 : -size, onSlice ? 200 : 2 * size, onSlice ? 0.1 : 1,
                [this, k, axis, size] {
                    const Mask* m = map_.mask();
                    return m && k < m->points.size() ? m->points[k][size_t(axis)] * size : 0.0;
                },
                [this, k, axis, size, id](double v) {
                    const Mask* m = map_.mask();
                    if (!m || k >= m->points.size())
                        return;
                    Points p = m->points;
                    p[k][size_t(axis)] = v / size;
                    session_.client().edit(SetMask{id, clamped(p), std::nullopt, std::nullopt});
                },
                onSlice ? "%" : "");
        };
        row(juce::String(int(k) + 1), {field(0), field(1)});
    }
    juce::TextButton* add = action("+ Point", [this, id] {
        const Mask* m = map_.mask();
        if (!m || m->points.size() >= kMaxMaskPoints)
            return;
        Points p = m->points;
        p.push_back({(p.back()[0] + p.front()[0]) / 2, (p.back()[1] + p.front()[1]) / 2});
        session_.client().edit(SetMask{id, p, std::nullopt, std::nullopt});
    });
    juce::TextButton* remove = action("\xe2\x88\x92 Point"_u, [this, id] {
        const Mask* m = map_.mask();
        if (!m || m->points.size() <= 3)
            return;
        Points p = m->points;
        p.pop_back();
        session_.client().edit(SetMask{id, p, std::nullopt, std::nullopt});
    });
    remove->setEnabled(mask.points.size() > 3);
    row("", {add, remove, action("Delete", [this] { map_.removeSelected(); })});
}

NumberField* MapInspector::field(const juce::String& label, int index) const
{
    for (const Row& r : rows_)
        if (r.label == label) {
            int seen = 0;
            for (juce::Component* part : r.parts)
                if (auto* f = dynamic_cast<NumberField*>(part); f && seen++ == index)
                    return f;
        }
    return nullptr;
}

juce::ComboBox* MapInspector::choiceFor(const juce::String& label) const
{
    for (const Row& r : rows_)
        if (r.label == label)
            for (juce::Component* part : r.parts)
                if (auto* box = dynamic_cast<juce::ComboBox*>(part))
                    return box;
    return nullptr;
}

juce::Button* MapInspector::button(const juce::String& text) const
{
    for (const auto& c : owned_)
        if (auto* b = dynamic_cast<juce::Button*>(c.get()); b && b->getButtonText() == text)
            return b;
    return nullptr;
}

void MapInspector::resized()
{
    int y = 4;
    for (const Row& r : rows_) {
        if (r.header) {
            y += 6 + kHeader;
            continue;
        }
        auto area = juce::Rectangle<int>(8, y, getWidth() - 16, kRow - 2);
        if (r.label.isNotEmpty())
            area.removeFromLeft(kLabelWidth);
        const int n = int(r.parts.size());
        const int width = n ? (area.getWidth() - 3 * (n - 1)) / n : 0;
        for (juce::Component* part : r.parts) {
            part->setBounds(area.removeFromLeft(width));
            area.removeFromLeft(3);
        }
        y += kRow;
    }
}

void MapInspector::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.panel);
    int y = 4;
    for (const Row& r : rows_) {
        if (r.header) {
            y += 6;
            g.setFont(Look::font(true));
            fillWithText(g, {0, y, getWidth(), kHeader}, p.header, p.white, r.label, juce::Justification::centredLeft, 8);
            y += kHeader;
            continue;
        }
        g.setFont(Look::font());
        g.setColour(p.muted);
        drawText(g, r.label, juce::Rectangle<int>(8, y, kLabelWidth - 4, kRow - 2), juce::Justification::centredLeft, true);
        y += kRow;
    }
}

} // namespace azl::ui
