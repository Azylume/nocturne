#pragma once

#include "ui/app/map_geometry.hpp"
#include "ui/app/widgets.hpp"

namespace azl::ui {

// Draws a live preview (or a still) letterboxed at 16:9.
void drawPicture(juce::Graphics& g, juce::Rectangle<int> area, const juce::Image& image);
// What a preview area shows while previews are off.
void drawPreviewsOff(juce::Graphics& g, juce::Rectangle<float> area);

// What the monitor's handles move: the selected layer's Transform effect, or the text clip it shows (or the selected
// one), whose position and size they set.
struct TransformTarget {
    uint64_t layer = 0;
    // The Transform effect, 0 while the layer has none (a drag adds one).
    uint64_t effect = 0;
    std::optional<uint64_t> textColumn;
    LayerTransform transform;
    TextClip text;
};
std::optional<TransformTarget> transformTarget(const Session& session);

// The output monitor: Composition, any group, or any screen with its mapping applied. The selected layer's Transform
// (or its text clip) has handles on it: drag inside to move, a corner to scale, the round handle to rotate (Shift
// snaps to 15 degrees). Each drag is one undo step.
class CompositionMonitor : public juce::Component, public PointerTips, private Session::View {
public:
    explicit CompositionMonitor(Session& session);
    ~CompositionMonitor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;
    int heightFor(int width) const;

    // Where a composition point shows on the monitor, if the view shows it.
    std::optional<juce::Point<float>> pixelOf(Point composition) const;
    // The box's corners, then the rotate handle for a Transform; empty without a target.
    std::vector<juce::Point<float>> handles() const;

private:
    enum class Grab { None, Move, Scale, Rotate };
    void sessionChanged(int changes) override;
    void rebuildViews();
    juce::Rectangle<float> picture() const;
    // The slice that shows the target's input on the screen the monitor shows; none for a plain view.
    std::optional<Slice> viewSlice(const TransformTarget& target) const;
    bool showsTarget(const TransformTarget& target) const;
    std::optional<Point> compositionAt(const TransformTarget& target, juce::Point<float> pixel, Point guess) const;
    std::optional<juce::Point<float>> pixelFor(const TransformTarget& target, Point composition) const;
    Points box(const TransformTarget& target) const;
    void send(bool force);

    Session& session_;
    juce::ComboBox view_;
    std::vector<std::string> sources_;
    Grab grab_ = Grab::None;
    std::optional<TransformTarget> start_;
    std::optional<TransformTarget> live_;
    Point startPoint_{};
    bool added_ = false;
    double lastSend_ = 0.0;
};

// The selected layer live, or the selected clip's moving preview (its still until that arrives, and while live clip
// previews are off) while it is not playing. Its header turns the engine's live clip previews on and off.
class PreviewMonitor : public juce::Component, private Session::View {
public:
    explicit PreviewMonitor(Session& session);
    ~PreviewMonitor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    int heightFor(int width) const;

    juce::TextButton& liveClipsButton() { return live_; }

private:
    void sessionChanged(int changes) override;

    Session& session_;
    juce::TextButton live_{"Move idle clips"};
};

// The channel mixes offered for a device with `count` input channels: all, each one, and each pair.
struct ChannelChoice {
    juce::String label;
    std::vector<int> channels;
};
std::vector<ChannelChoice> channelChoices(int count);
// Names each input channel that is silent or carries only noise, such as an unused preamp's hiss; empty when none.
juce::String channelHint(const std::vector<AudioLevels::Channel>& channels);

// The engine machine's audio input with its channels and gain, its level, spectrum and onsets after gain, then audio
// reactivity: each source (Kick, Snare, Hat, Bass, Low, Mid, High, Beat LFO) with the macros and parameters it drives,
// each with its live value. Clicking a parameter jumps to it; clicking a macro selects it in the Macros pane. The
// spectrum shows the Low, Mid and High bands with their live levels; dragging an edge's handle moves it, one undo step
// per drag.
class AudioPanel : public juce::Component, public PointerTips, private Session::View {
public:
    explicit AudioPanel(Session& session);
    ~AudioPanel() override;
    void paint(juce::Graphics&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;

    // Called after a macro row was clicked.
    std::function<void()> onMacro;

    struct Entry {
        juce::Rectangle<int> bounds;
        juce::String text;
        std::optional<ParamRef> param;
        int macroSlot = -1;
        std::string source;
        uint64_t modulator = 0;
    };
    std::vector<Entry> entries() const;
    // The sensitivity field of a source (kick, snare, hat, bass, lowBand, midBand, highBand or beat), in percent.
    NumberField& sensitivity(const std::string& source);
    // The handle of the Low/Mid (0) or Mid/High (1) edge on the spectrum.
    juce::Point<int> edgeHandle(int edge) const;
    juce::ComboBox& deviceBox() { return device_; }
    juce::ComboBox& channelBox() { return channels_; }
    juce::TextButton& autoGainButton() { return autoGain_; }
    juce::Slider& gainSlider() { return gain_; }
    juce::Rectangle<int> appliedGainArea() const;
    juce::Rectangle<int> levelArea() const;
    juce::Rectangle<int> spectrumArea() const;
    // The meters and readouts that move with the audio, without the controls over them.
    void repaintLive();
    // Kick, snare, hat.
    juce::Rectangle<int> onsetArea(int i) const;
    void resized() override;

private:
    void sessionChanged(int changes) override;
    void layoutFields();
    void paintModulator(juce::Graphics&, const Modulator&, juce::Rectangle<int> area) const;
    void refreshDevices();
    void refreshGain();
    void paintBands(juce::Graphics&, const AudioLevels& levels) const;
    float xOfHz(double hz) const;
    double hzAtX(float x) const;
    std::optional<int> edgeAt(juce::Point<int> point) const;

    Session& session_;
    std::optional<int> draggedEdge_;
    std::map<std::string, std::unique_ptr<NumberField>> sensitivity_;
    juce::ComboBox device_;
    std::vector<std::string> deviceIds_;
    std::string devicesSignature_;
    juce::ComboBox channels_;
    std::vector<ChannelChoice> channelChoices_;
    juce::TextButton autoGain_{"Auto"};
    juce::Slider gain_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft};
};

} // namespace azl::ui
