#include "ui/app/monitors.hpp"

#include "ui/app/look.hpp"

#include <cmath>

namespace azl::ui {

namespace {

constexpr int kHeader = 20;
constexpr int kFooter = 17;
constexpr int kHint = 14;

void paintFrame(juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& left, const juce::String& right,
                const juce::Image& image, bool off, int top = kHeader)
{
    const Palette& p = Look::palette();
    g.fillAll(p.panel);
    auto area = bounds.withTrimmedTop(top);
    auto footer = area.removeFromBottom(kFooter);
    if (off)
        drawPreviewsOff(g, area.toFloat());
    else
        drawPicture(g, area, image);
    g.setColour(p.header);
    g.fillRect(footer);
    g.setFont(Look::smallFont());
    g.setColour(p.muted);
    drawText(g, left, footer.reduced(6, 0), juce::Justification::centredLeft, true);
    drawText(g, right, footer.reduced(6, 0), juce::Justification::centredRight, true);
}

} // namespace

void drawPicture(juce::Graphics& g, juce::Rectangle<int> area, const juce::Image& image)
{
    g.setColour(juce::Colours::black);
    g.fillRect(area);
    if (image.isValid())
        g.drawImage(image, area.toFloat(), juce::RectanglePlacement::centred);
}

void drawPreviewsOff(juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setColour(juce::Colours::black);
    g.fillRect(area);
    g.setColour(Look::palette().muted);
    g.setFont(Look::font(true));
    drawText(g, "Previews off", area, juce::Justification::centred, true);
}

CompositionMonitor::CompositionMonitor(Session& session) : session_(session)
{
    view_.setTooltip("Which output or screen the monitor shows");
    view_.onChange = [this] {
        const int index = view_.getSelectedItemIndex();
        if (index >= 0 && index < int(sources_.size()))
            session_.setMonitorSource(sources_[size_t(index)]);
    };
    addAndMakeVisible(view_);
    session_.addView(this);
    rebuildViews();
}

CompositionMonitor::~CompositionMonitor()
{
    session_.removeView(this);
}

int CompositionMonitor::heightFor(int width) const
{
    return kHeader + width * 9 / 16 + kFooter;
}

void CompositionMonitor::rebuildViews()
{
    std::vector<std::string> sources{"composition"};
    juce::StringArray names{"Composition"};
    forEachLayer(session_.composition().layers, [&](const Layer& layer) {
        if (!layer.group)
            return;
        sources.push_back("layer:" + std::to_string(layer.id));
        names.add("Group: " + utf8(layer.name));
    });
    for (const Screen& screen : session_.composition().screens) {
        sources.push_back("screen:" + std::to_string(screen.id));
        names.add("Screen: " + utf8(screen.name.empty() ? "Screen " + std::to_string(screen.id) : screen.name));
    }
    juce::StringArray shown;
    for (int i = 0; i < view_.getNumItems(); ++i)
        shown.add(view_.getItemText(i));
    if (sources != sources_ || shown != names) {
        sources_ = sources;
        view_.clear(juce::dontSendNotification);
        view_.addItemList(names, 1);
    }
    auto at = std::find(sources_.begin(), sources_.end(), session_.monitorSource());
    if (at == sources_.end()) {
        session_.setMonitorSource("composition");
        at = sources_.begin();
    }
    view_.setSelectedItemIndex(int(at - sources_.begin()), juce::dontSendNotification);
}

void CompositionMonitor::sessionChanged(int changes)
{
    if (changes & (Session::Document | Session::Selection))
        rebuildViews();
    if (changes & (Session::Previews | Session::Clock | Session::Document | Session::Selection | Session::Connection))
        repaint();
}

void CompositionMonitor::resized()
{
    view_.setBounds(getLocalBounds().removeFromTop(kHeader));
}

void CompositionMonitor::paint(juce::Graphics& g)
{
    const double beat = session_.client().beatNow();
    const int bar = int(std::floor(beat / 4.0)) + 1;
    const int beatInBar = int(std::floor(beat)) % 4 + 1;
    const juce::String position = session_.client().connected()
                                      ? "bar " + juce::String(bar) + "." + juce::String(std::max(1, beatInBar))
                                      : juce::String("offline");
    paintFrame(g, getLocalBounds(), view_.getText(), position, session_.preview(session_.monitorSource()),
               !session_.previewsOn());
    const auto target = live_ ? live_ : transformTarget(session_);
    if (!target || !showsTarget(*target))
        return;
    const auto all = handles();
    if (all.size() < 4)
        return;
    const Palette& p = Look::palette();
    const juce::Colour colour = target->textColumn ? p.accent : p.live;
    juce::Path outline;
    outline.startNewSubPath(all[0]);
    for (size_t i = 1; i < 4; ++i)
        outline.lineTo(all[i]);
    outline.closeSubPath();
    g.setColour(colour);
    g.strokePath(outline, juce::PathStrokeType(1.2f));
    if (all.size() == 5) {
        g.drawLine({(all[0] + all[1]) / 2.0f, all[4]}, 1.0f);
        g.setColour(juce::Colour(0xff111111));
        g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(all[4]));
        g.setColour(colour);
        g.drawEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(all[4]), 1.2f);
    }
    for (size_t i = 0; i < 4; ++i) {
        const auto r = juce::Rectangle<float>(7.0f, 7.0f).withCentre(all[i]);
        g.setColour(colour);
        g.fillRect(r);
        g.setColour(juce::Colours::black);
        g.drawRect(r, 1.0f);
    }
}

std::optional<TransformTarget> transformTarget(const Session& session)
{
    const auto selected = session.selectedLayer();
    const Layer* layer = selected ? session.layer(*selected) : nullptr;
    if (!layer || layer->group)
        return std::nullopt;
    TransformTarget t;
    t.layer = layer->id;
    auto textAt = [&](uint64_t column) {
        auto it = layer->clips.find(column);
        if (it == layer->clips.end() || !it->second.text)
            return false;
        t.textColumn = column;
        t.text = *it->second.text;
        return true;
    };
    if (auto cell = session.selectedClip(); cell && cell->layer == layer->id && textAt(cell->column))
        return t;
    if (layer->playing && textAt(layer->playing->column))
        return t;
    for (const Effect& e : deckEffects(*layer, session.composition().deck))
        if (e.type == "transform") {
            t.effect = e.id;
            auto param = [&](const char* name, double fallback) {
                auto it = e.params.find(name);
                return it == e.params.end() ? fallback : it->second;
            };
            t.transform = {param("positionX", 0.0), param("positionY", 0.0), param("scale", 1.0), param("rotation", 0.0)};
            break;
        }
    return t;
}

juce::Rectangle<float> CompositionMonitor::picture() const
{
    auto area = getLocalBounds().withTrimmedTop(kHeader).withTrimmedBottom(kFooter).toFloat();
    const float height = std::min(area.getHeight(), area.getWidth() * 9.0f / 16.0f);
    return area.withSizeKeepingCentre(height * 16.0f / 9.0f, height);
}

std::optional<Slice> CompositionMonitor::viewSlice(const TransformTarget& target) const
{
    const std::string& source = session_.monitorSource();
    if (source.rfind("screen:", 0) != 0)
        return std::nullopt;
    const uint64_t id = std::stoull(source.substr(7));
    uint64_t group = 0;
    forEachLayer(session_.composition().layers, [&](const Layer& l) {
        for (const Layer& child : l.layers)
            if (child.id == target.layer)
                group = l.id;
    });
    for (const Screen& s : session_.composition().screens)
        if (s.id == id)
            for (const Slice& slice : s.slices)
                if (slice.input == 0 || slice.input == group)
                    return slice;
    return Slice{.input = UINT64_MAX};
}

bool CompositionMonitor::showsTarget(const TransformTarget& target) const
{
    const std::string& source = session_.monitorSource();
    if (source == "composition")
        return true;
    if (source.rfind("layer:", 0) == 0) {
        bool inside = false;
        forEachLayer(session_.composition().layers, [&](const Layer& l) {
            if ("layer:" + std::to_string(l.id) == source)
                for (const Layer& child : l.layers)
                    inside = inside || child.id == target.layer;
        });
        return inside;
    }
    const auto slice = viewSlice(target);
    return slice && slice->input != UINT64_MAX;
}

std::optional<juce::Point<float>> CompositionMonitor::pixelFor(const TransformTarget& target, Point composition) const
{
    if (!showsTarget(target))
        return std::nullopt;
    Point unit = composition;
    if (const auto slice = viewSlice(target))
        unit = sliceToScreen(*slice, composition);
    const auto r = picture();
    return juce::Point<float>(r.getX() + float(unit[0]) * r.getWidth(), r.getY() + float(unit[1]) * r.getHeight());
}

std::optional<juce::Point<float>> CompositionMonitor::pixelOf(Point composition) const
{
    const auto target = transformTarget(session_);
    return target ? pixelFor(*target, composition) : std::nullopt;
}

std::optional<Point> CompositionMonitor::compositionAt(const TransformTarget& target, juce::Point<float> pixel,
                                                       Point guess) const
{
    const auto r = picture();
    const Point unit{(pixel.x - r.getX()) / r.getWidth(), (pixel.y - r.getY()) / r.getHeight()};
    if (const auto slice = viewSlice(target))
        return screenToSlice(*slice, unit, guess);
    return unit;
}

Points CompositionMonitor::box(const TransformTarget& target) const
{
    return target.textColumn ? textBox(target.text, 16.0 / 9.0) : transformBox(target.transform, 16.0 / 9.0);
}

std::vector<juce::Point<float>> CompositionMonitor::handles() const
{
    const auto target = live_ ? live_ : transformTarget(session_);
    if (!target)
        return {};
    std::vector<juce::Point<float>> out;
    for (const Point& p : box(*target))
        if (const auto px = pixelFor(*target, p))
            out.push_back(*px);
    if (out.size() != 4 || target->textColumn)
        return out;
    const auto centre = (out[0] + out[2]) / 2.0f, mid = (out[0] + out[1]) / 2.0f;
    auto dir = mid - centre;
    const float len = dir.getDistanceFromOrigin();
    out.push_back(mid + (len > 0.001f ? dir / len : juce::Point<float>(0.0f, -1.0f)) * 16.0f);
    return out;
}

juce::String CompositionMonitor::tipAt(juce::Point<int> point)
{
    const auto target = transformTarget(session_);
    const auto pts = target && showsTarget(*target) ? handles() : std::vector<juce::Point<float>>{};
    const auto at = point.toFloat();
    if (pts.size() == 5 && pts[4].getDistanceFrom(at) < 8.0f)
        return "Drag to rotate the selection's Transform";
    for (size_t i = 0; i < std::min<size_t>(pts.size(), 4); ++i)
        if (pts[i].getDistanceFrom(at) < 7.0f)
            return "Drag a corner to scale the selection";
    if (pts.size() >= 4)
        return "Drag the box to move the selection; the corners scale it";
    return "What the chosen output shows, live";
}

void CompositionMonitor::mouseDown(const juce::MouseEvent& e)
{
    grab_ = Grab::None;
    const auto target = transformTarget(session_);
    if (!target || !showsTarget(*target))
        return;
    const auto pts = handles();
    if (pts.size() < 4)
        return;
    if (pts.size() == 5 && pts[4].getDistanceFrom(e.position) < 8.0f)
        grab_ = Grab::Rotate;
    for (size_t i = 0; grab_ == Grab::None && i < 4; ++i)
        if (pts[i].getDistanceFrom(e.position) < 7.0f)
            grab_ = Grab::Scale;
    if (grab_ == Grab::None) {
        juce::Path quad;
        quad.startNewSubPath(pts[0]);
        for (size_t i = 1; i < 4; ++i)
            quad.lineTo(pts[i]);
        quad.closeSubPath();
        if (quad.contains(e.position))
            grab_ = Grab::Move;
    }
    if (grab_ == Grab::None)
        return;
    const auto at = compositionAt(*target, e.position, centroid(box(*target)));
    if (!at) {
        grab_ = Grab::None;
        return;
    }
    start_ = target;
    live_ = target;
    startPoint_ = *at;
    added_ = false;
    lastSend_ = 0.0;
    session_.client().beginGesture();
}

void CompositionMonitor::mouseDrag(const juce::MouseEvent& e)
{
    if (grab_ == Grab::None || !start_)
        return;
    const auto at = compositionAt(*start_, e.position, startPoint_);
    if (!at)
        return;
    constexpr double aspect = 16.0 / 9.0;
    TransformTarget t = *start_;
    const Point c = centroid(box(*start_));
    auto distance = [&](Point p) { return std::hypot((p[0] - c[0]) * aspect, p[1] - c[1]); };
    const double ratio = distance(startPoint_) > 1e-6 ? distance(*at) / distance(startPoint_) : 1.0;
    const double dx = (*at)[0] - startPoint_[0], dy = (*at)[1] - startPoint_[1];
    if (t.textColumn) {
        if (grab_ == Grab::Move) {
            t.text.x = std::clamp(t.text.x + dx, -1.0, 2.0);
            t.text.y = std::clamp(t.text.y + dy, -1.0, 2.0);
        } else if (grab_ == Grab::Scale) {
            t.text.size = std::clamp(t.text.size * ratio, 0.01, 1.0);
        }
    } else if (grab_ == Grab::Move) {
        t.transform.x = std::clamp(t.transform.x + dx, -1.0, 1.0);
        t.transform.y = std::clamp(t.transform.y - dy, -1.0, 1.0);
    } else if (grab_ == Grab::Scale) {
        t.transform.scale = std::clamp(t.transform.scale * ratio, 0.0, 8.0);
    } else {
        auto iso = [&](Point p) { return Point{p[0] * aspect, p[1]}; };
        double degrees = start_->transform.rotation -
                         sweep(iso(c), iso(startPoint_), iso(*at), false) * 180.0 / 3.14159265358979323846;
        if (e.mods.isShiftDown())
            degrees = std::round(degrees / 15.0) * 15.0;
        t.transform.rotation = std::clamp(std::remainder(degrees, 360.0), -360.0, 360.0);
    }
    live_ = t;
    send(false);
    repaint();
}

void CompositionMonitor::send(bool force)
{
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    if (!live_ || (!force && now - lastSend_ < 1.0 / 30.0))
        return;
    lastSend_ = now;
    const TransformTarget& t = *live_;
    EngineClient& client = session_.client();
    if (t.textColumn) {
        SetClipText edit{t.layer, *t.textColumn};
        edit.x = t.text.x;
        edit.y = t.text.y;
        edit.size = t.text.size;
        client.edit(edit);
        return;
    }
    const std::map<std::string, double> params{{"positionX", t.transform.x},
                                               {"positionY", t.transform.y},
                                               {"scale", t.transform.scale},
                                               {"rotation", t.transform.rotation}};
    uint64_t effect = t.effect;
    if (!effect)
        if (const auto current = transformTarget(session_); current && current->layer == t.layer)
            effect = current->effect;
    if (effect)
        client.edit(SetEffectParams{effect, params});
    else if (!added_) {
        client.edit(addEffectTo(session_.composition(), {t.layer}, size_t(0), Effect{0, "transform", params}));
        added_ = true;
    }
}

void CompositionMonitor::mouseUp(const juce::MouseEvent&)
{
    if (grab_ == Grab::None)
        return;
    send(true);
    session_.client().endGesture();
    grab_ = Grab::None;
    start_.reset();
    live_.reset();
    repaint();
}

PreviewMonitor::PreviewMonitor(Session& session) : session_(session)
{
    live_.setClickingTogglesState(true);
    live_.setTooltip("A selected clip that is not playing moves here through its effects instead of showing its still. "
                     "The selected layer is always live. Moving clips cost the show machine some GPU: keep them off "
                     "while performing.");
    live_.onClick = [this] { session_.client().setClipPreviews(live_.getToggleState()); };
    addAndMakeVisible(live_);
    session_.addView(this);
    sessionChanged(Session::Previews);
}

PreviewMonitor::~PreviewMonitor()
{
    session_.removeView(this);
}

int PreviewMonitor::heightFor(int width) const
{
    return kHeader + kHint + width * 9 / 16 + kFooter;
}

void PreviewMonitor::resized()
{
    live_.setBounds(getLocalBounds().removeFromTop(kHeader).removeFromRight(112).reduced(2, 2));
}

void PreviewMonitor::sessionChanged(int changes)
{
    if (changes & Session::Previews) {
        live_.setToggleState(session_.client().clipPreviews(), juce::dontSendNotification);
        live_.setEnabled(session_.previewsOn());
    }
    if (changes & (Session::Previews | Session::Selection | Session::Stills | Session::Clock | Session::Document))
        repaint();
}

void PreviewMonitor::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    juce::String label = "-";
    juce::String state;
    juce::Image image;
    if (auto layerId = session_.selectedLayer()) {
        const Layer* layer = session_.layer(*layerId);
        label = layer ? "Layer " + utf8(layer->name) : juce::String("-");
        image = session_.preview(session_.previewSource());
        state = "live";
        if (auto cell = session_.selectedClip(); cell && layer) {
            auto clip = layer->clips.find(cell->column);
            if (clip != layer->clips.end()) {
                label << juce::String::fromUTF8(" \xc2\xb7 ") << utf8(clip->second.name);
                if (!session_.client().playing(cell->layer, cell->column)) {
                    image = session_.client().clipPreviews() ? session_.clipPreview(clip->second.hash) : juce::Image();
                    if (!image.isValid())
                        image = session_.still(clip->second.hash);
                    state = "not live";
                }
            }
        }
    }
    paintFrame(g, getLocalBounds(), label, state, image, !session_.previewsOn(), kHeader + kHint);
    g.setColour(p.muted);
    g.setFont(Look::font());
    drawText(g, "Preview Monitor", getLocalBounds().removeFromTop(kHeader).reduced(8, 0), juce::Justification::centredLeft);
    g.setFont(Look::smallFont());
    drawText(g, "Off: idle clips show a still. Layers stay live",
               getLocalBounds().withTrimmedTop(kHeader).removeFromTop(kHint).reduced(8, 0),
               juce::Justification::centredRight, true);
}

constexpr int kDeviceTop = kHeader + 4;
constexpr int kGainTop = kDeviceTop + 23;
constexpr int kLevelTop = kGainTop + 23;
constexpr int kSpectrumTop = kLevelTop + 17;
constexpr int kChannelsWidth = 64;
constexpr int kAutoWidth = 44;
constexpr int kAppliedWidth = 76;
constexpr double kMinGainDb = -24.0;
constexpr double kMaxGainDb = 48.0;
constexpr double kSilentDb = -70.0;
constexpr double kNoiseDb = -40.0;
constexpr double kNoiseOnly = 0.5;
constexpr int kOnsetsTop = kSpectrumTop + 47;
constexpr int kAnalysisBottom = kOnsetsTop + 22;

constexpr const char* kPanelSources[] = {"kick", "snare", "hat", "bass", "lowBand", "midBand", "highBand", "beat"};
constexpr const char* kPanelNames[] = {"Kick", "Snare", "Hat", "Bass", "Low", "Mid", "High", "Beat LFO"};
constexpr const char* kSplitSources[] = {"lowBand", "midBand", "highBand"};
constexpr const char* kSplitNames[] = {"Low", "Mid", "High"};
constexpr double kSpectrumLowHz = 30.0;
constexpr double kSpectrumHighHz = 16000.0;
constexpr int kEdgeGrab = 5;

// The document names the beat LFO's sensitivity "lfo".
std::string sensitivityKey(const std::string& source)
{
    return source == "beat" ? "lfo" : source;
}

juce::String hzText(double hz)
{
    return hz < 1000.0 ? juce::String(juce::roundToInt(hz)) + " Hz" : juce::String(hz / 1000.0, 1) + " kHz";
}

AudioPanel::AudioPanel(Session& session) : session_(session)
{
    for (const char* source : kPanelSources) {
        auto field = std::make_unique<NumberField>(session_, 0.0, kMaxSensitivity * 100.0, 5.0, "%");
        field->setTooltip("Sensitivity, 0 to " + juce::String(kMaxSensitivity * 100.0, 0) +
                          "%: how hard this source drives what it modulates; drag sideways or double-click");
        field->onChange = [this, key = sensitivityKey(source)](double percent) {
            session_.client().edit(SetSensitivity{key, percent / 100.0});
        };
        addAndMakeVisible(*field);
        sensitivity_[source] = std::move(field);
    }
    device_.setTooltip("The engine machine's audio input. Its gain and channels are remembered per device and come back "
                       "when it is selected, plugged back in or the engine restarts");
    device_.onChange = [this] {
        const int i = device_.getSelectedItemIndex();
        if (i >= 0 && i < int(deviceIds_.size()))
            session_.client().selectAudioInput(deviceIds_[size_t(i)]);
    };
    addAndMakeVisible(device_);
    channels_.setTooltip("Input channels mixed into the analysis. Leave out a channel with nothing plugged in: an idle "
                         "preamp adds its hiss to the mix");
    channels_.onChange = [this] {
        const int i = channels_.getSelectedItemIndex();
        const int device = device_.getSelectedItemIndex();
        if (i >= 0 && i < int(channelChoices_.size()) && device >= 0 && device < int(deviceIds_.size()))
            session_.client().selectAudioInput(deviceIds_[size_t(device)], channelChoices_[size_t(i)].channels);
    };
    addAndMakeVisible(channels_);
    autoGain_.setClickingTogglesState(true);
    autoGain_.setTooltip("Auto gain: each feature follows its own recent peak, so quiet and loud inputs both fill the "
                         "meters. Off, the gain slider sets a fixed gain");
    autoGain_.onClick = [this] { session_.client().setAudioGain({autoGain_.getToggleState(), gain_.getValue()}); };
    addAndMakeVisible(autoGain_);
    const Palette& p = Look::palette();
    gain_.setRange(kMinGainDb, kMaxGainDb, 0.5);
    gain_.setTextValueSuffix(" dB");
    gain_.setColour(juce::Slider::trackColourId, p.meter);
    gain_.setColour(juce::Slider::backgroundColourId, p.field);
    gain_.setColour(juce::Slider::textBoxTextColourId, p.text);
    gain_.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    gain_.setTooltip("Manual input gain, " + juce::String(kMinGainDb, 0) + " to +" + juce::String(kMaxGainDb, 0) +
                     " dB, used while Auto is off; drag, or double-click to type");
    gain_.onValueChange = [this] { session_.client().setAudioGain({false, gain_.getValue()}); };
    addAndMakeVisible(gain_);
    session_.addView(this);
    refreshDevices();
    refreshGain();
}

std::vector<ChannelChoice> channelChoices(int count)
{
    std::vector<ChannelChoice> choices{{count == 1 ? "1" : count == 2 ? "1+2" : "All", {}}};
    for (int c = 1; c <= count && count > 1; ++c)
        choices.push_back({juce::String(c), {c}});
    for (int c = 1; c + 1 <= count && count > 2; c += 2)
        choices.push_back({juce::String(c) + "+" + juce::String(c + 1), {c, c + 1}});
    return choices;
}

juce::String channelHint(const std::vector<AudioLevels::Channel>& channels)
{
    juce::StringArray hints;
    for (size_t i = 0; i < channels.size(); ++i) {
        const AudioLevels::Channel& c = channels[i];
        if (c.levelDb < kSilentDb)
            hints.add("Ch " + juce::String(i + 1) + " silent");
        else if (c.levelDb < kNoiseDb && c.noise > kNoiseOnly)
            hints.add("Ch " + juce::String(i + 1) + " noise only");
    }
    return hints.joinIntoString(", ");
}

juce::Rectangle<int> AudioPanel::appliedGainArea() const
{
    return {getWidth() - 6 - kAppliedWidth, kGainTop, kAppliedWidth, 20};
}

juce::Rectangle<int> AudioPanel::levelArea() const
{
    return {6, kLevelTop, getWidth() - 12, 14};
}

void AudioPanel::refreshGain()
{
    const AudioGain gain = session_.client().audioGain();
    autoGain_.setToggleState(gain.autoGain, juce::dontSendNotification);
    gain_.setEnabled(!gain.autoGain);
    if (!gain_.isMouseButtonDown())
        gain_.setValue(gain.gainDb, juce::dontSendNotification);
}

juce::Rectangle<int> AudioPanel::spectrumArea() const
{
    return {6, kSpectrumTop, getWidth() - 12, 44};
}

juce::Rectangle<int> AudioPanel::onsetArea(int i) const
{
    const int width = (getWidth() - 12 - 2) / 3;
    return {6 + i * (width + 1), kOnsetsTop, width, 18};
}

// The default input, each device, and off; the one the engine reports is shown.
void AudioPanel::refreshDevices()
{
    const auto devices = session_.client().audioDevices();
    const AudioInputState input = session_.client().audioInput();
    juce::StringArray mixed;
    for (int c : input.channels)
        mixed.add(juce::String(c));
    std::string signature = input.name + "|" + mixed.joinIntoString("+").toStdString();
    for (const AudioDevice& d : devices)
        signature += "|" + d.uid + d.name + std::to_string(d.channels);
    if (signature == devicesSignature_ && device_.getNumItems() > 0)
        return;
    devicesSignature_ = signature;
    deviceIds_ = {""};
    juce::StringArray names{"Default input"};
    int selected = 0;
    int count = 0;
    for (const AudioDevice& d : devices) {
        if (d.name == input.name)
            selected = names.size();
        if (d.uid == input.uid)
            count = d.channels;
        deviceIds_.push_back(d.uid);
        names.add(utf8(d.name));
    }
    deviceIds_.push_back("off");
    names.add("Off");
    device_.clear(juce::dontSendNotification);
    device_.addItemList(names, 1);
    device_.setSelectedItemIndex(selected, juce::dontSendNotification);

    channelChoices_ = count > 0 ? channelChoices(count) : std::vector<ChannelChoice>{};
    channels_.clear(juce::dontSendNotification);
    for (size_t i = 0; i < channelChoices_.size(); ++i) {
        channels_.addItem(channelChoices_[i].label, int(i) + 1);
        if (channelChoices_[i].channels == input.channels)
            channels_.setSelectedItemIndex(int(i), juce::dontSendNotification);
    }
    if (channels_.getSelectedItemIndex() < 0 && count > 0)
        channels_.setText(mixed.joinIntoString("+"), juce::dontSendNotification);
    channels_.setEnabled(count > 1);
}

NumberField& AudioPanel::sensitivity(const std::string& source)
{
    return *sensitivity_.at(source);
}

void AudioPanel::resized()
{
    device_.setBounds(6, kDeviceTop, getWidth() - 16 - kChannelsWidth, 20);
    channels_.setBounds(getWidth() - 6 - kChannelsWidth, kDeviceTop, kChannelsWidth, 20);
    autoGain_.setBounds(6, kGainTop, kAutoWidth, 20);
    gain_.setBounds(10 + kAutoWidth, kGainTop, appliedGainArea().getX() - 14 - kAutoWidth, 20);
    layoutFields();
}

void AudioPanel::layoutFields()
{
    const auto& levels = session_.composition().sensitivity;
    for (const Entry& entry : entries()) {
        if (entry.source.empty())
            continue;
        NumberField& field = *sensitivity_.at(entry.source);
        field.setBounds(entry.bounds.withTrimmedLeft(entry.bounds.getWidth() - 52).reduced(2, 1));
        auto it = levels.find(sensitivityKey(entry.source));
        field.setValue(std::round((it == levels.end() ? 1.0 : it->second) * 100.0));
    }
}

AudioPanel::~AudioPanel()
{
    session_.removeView(this);
}

void AudioPanel::sessionChanged(int changes)
{
    if (changes & Session::Document)
        layoutFields();
    if (changes & (Session::Audio | Session::Connection)) {
        refreshDevices();
        refreshGain();
    }
    if (changes & Session::Document)
        repaint();
    else if (changes & (Session::Audio | Session::Clock | Session::Modulation))
        repaintLive();
}

void AudioPanel::repaintLive()
{
    repaint(getLocalBounds().removeFromTop(kHeader));
    repaint(appliedGainArea());
    repaint(levelArea());
    repaint(spectrumArea());
    for (const Entry& entry : entries())
        repaint(entry.source.empty() ? entry.bounds : entry.bounds.withTrimmedRight(56));
}

std::vector<AudioPanel::Entry> AudioPanel::entries() const
{
    const Composition& composition = session_.composition();
    const auto slots = macroSlots(composition);
    std::vector<Entry> result;
    int y = kAnalysisBottom + 18;
    const int width = getWidth();
    for (size_t s = 0; s < std::size(kPanelSources); ++s) {
        result.push_back({{0, y, width, 18}, kPanelNames[s], std::nullopt, -1, kPanelSources[s]});
        y += 19;
        bool any = false;
        for (const Modulator& m : composition.modulators) {
            if (animationSource(m) != kPanelSources[s])
                continue;
            any = true;
            Entry entry{{0, y, width, 16}, {}, std::nullopt, -1, {}, m.id};
            if (m.target.macro) {
                for (int i = 0; i < kMacroSlots; ++i)
                    if (slots[size_t(i)] && slots[size_t(i)]->id == m.target.macro) {
                        entry.macroSlot = i;
                        entry.text = juce::String::fromUTF8("\xe2\xa6\xbf ") + utf8(slots[size_t(i)]->name);
                    }
                if (entry.macroSlot < 0)
                    entry.text = "group macro";
            } else {
                entry.param = m.target.param;
                entry.text = utf8(paramLabel(composition, m.target.param));
            }
            result.push_back(entry);
            y += 16;
        }
        if (!any) {
            result.push_back({{0, y, width, 16}, "nothing", std::nullopt, -1, {}});
            y += 16;
        }
        y += 3;
    }
    return result;
}

void AudioPanel::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.panel);
    auto area = getLocalBounds();
    g.setFont(Look::font());
    const AudioLevels levels = session_.client().audio();
    const juce::String state = levels.values.empty() ? "no audio input" : levels.live ? "live" : "silent";
    fillWithText(g, area.removeFromTop(kHeader), p.header, p.muted, "Audio", juce::Justification::centredLeft, 8);
    g.setColour(levels.live ? p.live : p.muted);
    drawText(g, state, getLocalBounds().removeFromTop(kHeader).reduced(8, 0), juce::Justification::centredRight);
    g.setColour(levels.values.empty() ? p.muted : p.text);
    drawText(g, levels.values.empty() ? juce::String() : juce::String::formatted("now %+.1f dB", levels.gainDb),
               appliedGainArea(), juce::Justification::centredRight);
    auto level = levelArea();
    const juce::String hint = channelHint(levels.channels);
    g.setFont(Look::smallFont());
    g.setColour(p.fault);
    drawText(g, hint, level.removeFromRight(hint.isEmpty() ? 0 : level.getWidth() / 2), juce::Justification::centredRight,
               true);
    const auto it = levels.values.find("level");
    drawBar(g, level.reduced(0, 3).withTrimmedRight(hint.isEmpty() ? 0 : 6),
            it == levels.values.end() ? 0.0f : float(it->second), p.meter);
    g.setFont(Look::font());
    const auto spectrum = spectrumArea();
    g.setColour(juce::Colours::black);
    g.fillRect(spectrum);
    const size_t bands = levels.spectrum.size();
    for (size_t i = 0; i < bands; ++i) {
        const float x0 = float(spectrum.getX()) + float(spectrum.getWidth()) * float(i) / float(bands);
        const float w = float(spectrum.getWidth()) / float(bands) - 2.0f;
        const float h = float(spectrum.getHeight()) * float(std::clamp(levels.spectrum[i], 0.0, 1.0));
        g.setColour(i < bands / 6 ? p.accent : p.live);
        g.fillRect(x0 + 1.0f, float(spectrum.getBottom()) - h, w, h);
    }
    paintBands(g, levels);
    for (int i = 0; i < 3; ++i) {
        static const char* const onsets[] = {"kick", "snare", "hat"};
        static const char* const names[] = {"Kick", "Snare", "Hat"};
        const auto it = levels.onsets.find(onsets[i]);
        const bool hit = it != levels.onsets.end() && it->second;
        fillWithText(g, onsetArea(i), hit ? p.accent : p.button, hit ? juce::Colour(0xff1a1405) : p.muted, names[i],
                     juce::Justification::centred, 0);
    }
    area.setTop(kAnalysisBottom);
    g.setColour(p.muted);
    g.setFont(Look::smallFont());
    drawText(g, "Audio reactivity: click a row to jump to it", area.removeFromTop(18).reduced(8, 0),
               juce::Justification::centredLeft, true);
    for (const Entry& entry : entries()) {
        if (!entry.source.empty()) {
            fillWithText(g, entry.bounds, p.cell, p.text, entry.text, juce::Justification::centredLeft, 8);
            if (entry.source != "beat") {
                auto it = levels.values.find(entry.source);
                drawBar(g, entry.bounds.withTrimmedLeft(70).withTrimmedRight(52).reduced(8, 5),
                        it == levels.values.end() ? 0.0f : float(it->second), p.meter);
            }
            continue;
        }
        auto text = entry.bounds.reduced(16, 0);
        const auto& modulators = session_.composition().modulators;
        const auto m = std::find_if(modulators.begin(), modulators.end(), [&](const Modulator& m) { return m.id == entry.modulator; });
        if (m != modulators.end())
            paintModulator(g, *m, text.removeFromRight(text.getWidth() * 2 / 5).reduced(0, 2));
        g.setFont(Look::smallFont());
        g.setColour(p.muted);
        if (entry.macroSlot >= 0)
            drawText(g, "macro", text.removeFromRight(40), juce::Justification::centredRight);
        g.setColour(entry.param || entry.macroSlot >= 0 ? p.text : p.muted);
        drawText(g, entry.text, text, juce::Justification::centredLeft, true);
    }
}

float AudioPanel::xOfHz(double hz) const
{
    const auto area = spectrumArea().toFloat();
    const double t = std::log(hz / kSpectrumLowHz) / std::log(kSpectrumHighHz / kSpectrumLowHz);
    return area.getX() + area.getWidth() * float(std::clamp(t, 0.0, 1.0));
}

double AudioPanel::hzAtX(float x) const
{
    const auto area = spectrumArea().toFloat();
    const double t = std::clamp(double((x - area.getX()) / area.getWidth()), 0.0, 1.0);
    return kSpectrumLowHz * std::pow(kSpectrumHighHz / kSpectrumLowHz, t);
}

juce::Point<int> AudioPanel::edgeHandle(int edge) const
{
    return {juce::roundToInt(xOfHz(session_.composition().bandEdges[size_t(edge)])), spectrumArea().getCentreY()};
}

std::optional<int> AudioPanel::edgeAt(juce::Point<int> point) const
{
    if (!spectrumArea().contains(point))
        return std::nullopt;
    std::optional<int> nearest;
    for (int edge = 0; edge < 2; ++edge) {
        const int distance = std::abs(point.x - edgeHandle(edge).x);
        if (distance <= kEdgeGrab && (!nearest || distance < std::abs(point.x - edgeHandle(*nearest).x)))
            nearest = edge;
    }
    return nearest;
}

// Each band's name and live level along the top of its span, and a handle on each edge.
void AudioPanel::paintBands(juce::Graphics& g, const AudioLevels& levels) const
{
    const Palette& p = Look::palette();
    const auto area = spectrumArea().toFloat();
    const auto& edges = session_.composition().bandEdges;
    const float bounds[] = {area.getX(), xOfHz(edges[0]), xOfHz(edges[1]), area.getRight()};
    g.setFont(Look::smallFont());
    for (size_t b = 0; b < std::size(kSplitSources); ++b) {
        const auto span = juce::Rectangle<float>(bounds[b], area.getY(), bounds[b + 1] - bounds[b], 12.0f);
        const auto it = levels.values.find(kSplitSources[b]);
        const float level = it == levels.values.end() ? 0.0f : float(std::clamp(it->second, 0.0, 1.0));
        g.setColour(p.meter.withAlpha(0.8f));
        g.fillRect(span.withHeight(3.0f).withWidth(span.getWidth() * level));
        g.setColour(p.text);
        drawText(g, kSplitNames[b], span.withTrimmedTop(3.0f).reduced(4.0f, 0.0f).toNearestInt(),
                 juce::Justification::centredLeft, true);
    }
    for (int edge = 0; edge < 2; ++edge) {
        const float x = bounds[edge + 1];
        g.setColour(p.white.withAlpha(draggedEdge_ == edge ? 1.0f : 0.7f));
        g.fillRect(x - 1.0f, area.getY(), 2.0f, area.getHeight());
        g.fillRect(x - 3.0f, area.getCentreY() - 6.0f, 6.0f, 12.0f);
        drawText(g, hzText(edges[size_t(edge)]),
                 juce::Rectangle<float>(x + 4.0f, area.getBottom() - 12.0f, 60.0f, 12.0f).toNearestInt(),
                 juce::Justification::centredLeft, true);
    }
}

void AudioPanel::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(edgeAt(e.getPosition()) ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void AudioPanel::mouseDown(const juce::MouseEvent& e)
{
    draggedEdge_ = edgeAt(e.getPosition());
    if (draggedEdge_)
        session_.client().beginGesture();
}

void AudioPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (!draggedEdge_)
        return;
    std::array<double, 2> edges = session_.composition().bandEdges;
    const double hz = std::round(hzAtX(float(e.position.x)));
    if (*draggedEdge_ == 0)
        edges[0] = std::clamp(hz, kMinBandEdge, std::floor(edges[1] / kMinMidBandRatio));
    else
        edges[1] = std::clamp(hz, std::ceil(edges[0] * kMinMidBandRatio), kMaxBandEdge);
    if (edges != session_.composition().bandEdges)
        session_.client().edit(SetBandEdges{edges});
}

// An audio source as its live level; a beat LFO as its target over one period, with a playhead at its phase.
void AudioPanel::paintModulator(juce::Graphics& g, const Modulator& m, juce::Rectangle<int> area) const
{
    const Palette& p = Look::palette();
    if (m.source == ModulatorSource::Audio) {
        const auto& state = session_.modulation().modulators;
        const auto live = state.find(m.id);
        drawBar(g, area, live == state.end() ? 0.0f : float(live->second), p.meter);
        return;
    }
    g.setColour(p.field);
    g.fillRect(area);
    const auto& levels = session_.composition().sensitivity;
    const auto it = levels.find("lfo");
    const double sensitivity = it == levels.end() ? 1.0 : it->second;
    const auto bounds = area.toFloat();
    auto yAt = [&](double x) {
        const double source = std::clamp(lfoAt(m.shape, x + m.phase, m.points) * sensitivity, 0.0, 1.0);
        return bounds.getBottom() - bounds.getHeight() * float(modulatedAt(session_.composition(), m, source));
    };
    auto xAt = [&](double x) { return bounds.getX() + bounds.getWidth() * float(x); };
    juce::Path shape;
    shape.startNewSubPath(xAt(0.0), yAt(0.0));
    for (int i = 1; i <= area.getWidth(); ++i)
        shape.lineTo(xAt(double(i) / area.getWidth()), yAt(double(i) / area.getWidth()));
    g.setColour(p.accent);
    g.strokePath(shape, juce::PathStrokeType(1.0f));
    const double periods = session_.client().beatNow() / std::max(m.beats, kMinLfoBeats);
    const double head = periods - std::floor(periods);
    g.setColour(p.white);
    g.drawVerticalLine(juce::roundToInt(xAt(head)), bounds.getY(), bounds.getBottom());
    g.fillEllipse(xAt(head) - 2.0f, yAt(head) - 2.0f, 4.0f, 4.0f);
}

juce::String AudioPanel::tipAt(juce::Point<int> point)
{
    if (appliedGainArea().contains(point))
        return "The gain applied now, also while Auto runs";
    if (levelArea().contains(point))
        return "Input level after gain, and a warning when an input channel is silent or carries only noise";
    if (spectrumArea().contains(point))
        return "Spectrum after gain, 30 Hz to 16 kHz, split into the Low, Mid and High sources; drag a handle to move "
               "an edge";
    for (const Entry& entry : entries())
        if (entry.bounds.contains(point)) {
            if (entry.param)
                return "Click to show this parameter in its pane";
            if (entry.macroSlot >= 0)
                return "Click to edit this macro in the Macros tab";
        }
    return {};
}

void AudioPanel::mouseUp(const juce::MouseEvent& e)
{
    if (draggedEdge_) {
        draggedEdge_.reset();
        session_.client().endGesture();
        repaint(spectrumArea());
        return;
    }
    for (const Entry& entry : entries()) {
        if (!entry.bounds.contains(e.getPosition()))
            continue;
        if (entry.param)
            session_.goToParam(*entry.param);
        else if (entry.macroSlot >= 0) {
            session_.selectMacro(entry.macroSlot);
            if (onMacro)
                onMacro();
        }
    }
}

} // namespace azl::ui
