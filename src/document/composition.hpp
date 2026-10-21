#pragma once

#include "document/mapping.hpp"
#include "document/modulation.hpp"
#include "document/text.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace azl {

// Timeline clips run at their native frame rate; BPM-synced clips stretch to their beat count on the beat clock.
enum class SyncMode { Timeline, Bpm };
// As in Resolume: once-and-clear shows nothing after the end, once-and-hold keeps the last frame. Reverse loops
// backward; Random Jump loops forward and jumps on the clip's beat grid.
enum class PlayMode { Loop, Bounce, OnceClear, OnceHold, Reverse, RandomJump };

struct Transport {
    SyncMode sync = SyncMode::Bpm;
    PlayMode playMode = PlayMode::Loop;
    // The clip's length in beats when BPM-synced; 0 picks the power of two that puts it between 80 and 160 BPM.
    int beats = 0;
    // Random Jump: every `jumpInterval` clip beats (a power of two, 1/8..8) the clip jumps up to `jumpDistance` beats
    // (0..1024) either way.
    double jumpInterval = 8.0;
    double jumpDistance = 2.0;
    // The clip plays only between `in` and `out` (0..1 of its frames, in < out), `speed` times as fast (0..4).
    double speed = 1.0;
    double in = 0.0;
    double out = 1.0;

    bool operator==(const Transport&) const = default;
};

// Autopilot moves a layer on to the next clip of its deck, or a random one, every `bars` bars; a clip's own step
// overrides them. The steps run on the bar from the layer's last trigger. A trigger while Autopilot plays the layer
// holds its clip for `hold` bars (0: one step), then Autopilot moves on from it. An effect's Autopilot steps through
// its steps the same way.
enum class AutopilotMode { Off, Next, Random };

inline constexpr int kMaxStepBars = 64;

struct Autopilot {
    AutopilotMode mode = AutopilotMode::Off;
    int bars = 4;
    int hold = 16;

    bool operator==(const Autopilot&) const = default;
};

// A snapshot of an effect's parameters that its Autopilot steps to; a parameter it leaves out keeps its value.
struct EffectStep {
    std::string name;
    std::map<std::string, double> params;

    bool operator==(const EffectStep&) const = default;
};

// The step an effect was triggered to last, from `beat` on the beat clock.
struct StepPlay {
    size_t step = 0;
    std::optional<double> beat;

    bool operator==(const StepPlay&) const = default;
};

inline constexpr size_t kMaxEffectSteps = 64;
inline constexpr double kDefaultStepTransition = 1.0;

// An instance of a registered effect. Ids are unique within the composition; params hold every parameter of the type.
struct Effect {
    uint64_t id = 0;
    std::string type;
    std::map<std::string, double> params;
    // Renders as if its dry/wet were 0, with no side passes; the chain and its pipelines stay as they are.
    bool bypass = false;
    // The preset it was set from last, by name; empty for none.
    std::string preset{};
    // Colour groups (by key) that follow these colours of the active palette, by colour id.
    std::map<std::string, uint64_t> links{};
    // Moving to a step glides there over `transition` beats: numbers and colours glide, choices switch halfway.
    std::vector<EffectStep> steps{};
    Autopilot autopilot{};
    double transition = kDefaultStepTransition;
    std::optional<StepPlay> playing{};

    bool operator==(const Effect&) const = default;
};

// Media is referenced by content hash, never by path, so a composition plays on any machine whose Media Library holds
// the file. A text clip has no media: its hash is empty and `text` is set. A generator clip has neither: its hash is
// empty and its first effect is a generator, which draws its picture.
struct ClipRef {
    std::string hash;
    std::string name;
    Transport transport;
    // The bars this clip plays under Autopilot; 0 keeps the layer's step.
    int step = 0;
    // The beats of the transition into this clip; none keeps the layer's.
    std::optional<double> transition{};
    // Applied in order to the clip's picture, before its layer's chain. The clip keeps it through a transition.
    std::vector<Effect> effects{};
    std::optional<TextClip> text{};
    // The quantisation its triggers land on, in beats as SetQuantise takes them; none keeps the composition's.
    std::optional<int> quantise{};

    bool operator==(const ClipRef&) const = default;
};

bool isGeneratorClip(const ClipRef& clip);

// How a layer combines with the picture below it. The first four blend in fixed function; the others read the
// composition so far.
enum class BlendMode { Normal, Add, Screen, Multiply, Difference, Subtract, Exclusion, Lighten, Darken, Overlay,
                       HardLight, SoftLight, Dodge, Burn };

inline constexpr int kBlendModeCount = 14;

const char* blendModeName(BlendMode mode);
// Throws ProtocolError for an unknown name.
BlendMode blendModeFromName(const std::string& name);

// Beats computed from times land a hair off a beat; this keeps them on it.
inline constexpr double kBeatEpsilon = 1e-9;

// The cell a layer plays: its clip in `column`, from `beat` on the beat clock on, or from the first frame that renders
// it when there is no beat.
struct Play {
    uint64_t column = 0;
    std::optional<double> beat;

    bool operator==(const Play&) const = default;
};

// How a layer moves from one clip to the next, over a number of beats on the beat clock (0 cuts): a crossfade, a
// left-to-right wipe, scattered pixels switching over, or the new clip pushing the old one out to the left; or one of
// the imported ISF transitions (effects/isf.hpp). From kFirstUserTransition on, the ISF transitions of the engine's
// user folder, numbered per process (userTransitionMode); the document names them by type.
enum class TransitionMode : uint32_t { Alpha, Wipe, Dissolve, Push, WindowSlice, Radial, RandomSquares };

inline constexpr uint32_t kFirstUserTransition = 64;
inline constexpr double kMaxTransitionBeats = 64.0;

// A user ISF transition's mode in this process by its type ("userMyWipe"), given on first use and kept, so a document
// naming one that is not loaded keeps it. Any thread. Throws ProtocolError for a name that is not a user type.
TransitionMode userTransitionMode(const std::string& type);
// The document's name of a mode: "alpha" ... "randomSquares", or a user transition's type.
std::string transitionName(TransitionMode mode);
// Throws ProtocolError for an unknown name.
TransitionMode transitionFromName(const std::string& name);

struct Transition {
    TransitionMode mode = TransitionMode::Alpha;
    double beats = 0.0;

    bool operator==(const Transition&) const = default;
};

// A musical part of the current track.
enum class Section { Intro, Build, Drop, Breakdown, Outro };

const char* sectionName(Section section);
// Throws ProtocolError for an unknown name.
Section sectionFromName(const std::string& name);

// A Section starting on a beat of the beat clock; it lasts until the next mark.
struct SectionMark {
    Section section = Section::Intro;
    double beat = 0.0;

    bool operator==(const SectionMark&) const = default;
};

// A Cue's firing at `beat`, taken back before it fired.
struct CueSkip {
    uint64_t cue = 0;
    double beat = 0.0;

    bool operator==(const CueSkip&) const = default;
};

// The live timeline of the track playing: its Section marks in beat order, past and scheduled, and the Cue firings
// taken back. It is playback: it changes only by SetTimeline, a live edit outside the undo history.
struct Timeline {
    std::vector<SectionMark> sections;
    std::vector<CueSkip> skips;

    bool operator==(const Timeline&) const = default;
};

// Changes only the fields that are set.
struct CueAutopilot {
    uint64_t layer = 0;
    std::optional<AutopilotMode> mode;
    std::optional<int> bars;

    bool operator==(const CueAutopilot&) const = default;
};

// The macro moves to `value` over `bars` bars from the Cue's firing (0 jumps), from the value it had then.
struct CueMacro {
    uint64_t macro = 0;
    double value = 0.0;
    int bars = 0;

    bool operator==(const CueMacro&) const = default;
};

// Fires a column when the Cue fires, as a column trigger on that beat: the column with id `column` (column ids are
// unique across decks, so it names its deck too), or with `index` the index-th column (0 is the first) of whichever
// deck is shown when it fires.
struct CueColumn {
    uint64_t column = 0;
    std::optional<size_t> index;

    bool operator==(const CueColumn&) const = default;
};

// Changes only the fields that are set of an effect's Autopilot; with `step`, the effect moves to that step when the
// Cue fires.
struct CueEffect {
    uint64_t effect = 0;
    std::optional<AutopilotMode> mode;
    std::optional<int> bars;
    std::optional<size_t> step{};

    bool operator==(const CueEffect&) const = default;
};

inline constexpr int kMaxCueRampBars = 64;

// A time of day on the venue machine's clock, in minutes from local midnight: on `date` (YYYY-MM-DD) only, or daily.
struct CueAt {
    int minute = 0;
    std::optional<std::string> date{};

    bool operator==(const CueAt&) const = default;
};

// A change scheduled at a musical position: bar `bar` of every `section` (bar 1 is the Section's first), or without a
// section bar `bar` of the beat clock (bar 1 starts on beat 0). With `every` it fires again every that many bars from
// there, within its Section or on the beat clock. Its values hold until the Section it fired in ends; then the
// document's own values return. A later Cue in the same Section wins. A Cue switched off never fires.
//
// With `at` it fires at that time of day instead, on the next bar after it, as live changes: it shows `deck` and fires
// its column, and carries nothing else. `gig` is the date of the Gig whose lineup made it, empty for a hand-made Cue.
// Only such a Cue shows a deck: a deck switches at a time of day or by hand, never on a Section or a bar.
//
// With `prepared` (a Prepare change, protocol/prepare.hpp) it fires at the next deck switch instead: the change applies
// as one undo step and the Cue is gone. It carries nothing else.
struct Cue {
    uint64_t id = 0;
    std::string name;
    std::optional<Section> section;
    int bar = 1;
    std::vector<CueAutopilot> autopilot;
    std::vector<CueMacro> macros;
    std::optional<CueColumn> column{};
    std::vector<CueEffect> effects{};
    bool enabled = true;
    std::optional<int> every{};
    std::optional<CueAt> at{};
    std::optional<uint64_t> deck{};
    std::string gig{};
    std::optional<nlohmann::json> prepared{};

    bool operator==(const Cue&) const = default;
};

inline constexpr size_t kMaxCues = 256;
inline constexpr int kMaxCueBar = 100000;
inline constexpr size_t kMaxTimelineEntries = 4096;

// When a Gig runs on the venue machine's clock: from `start` on `date` to `end` (minutes from local midnight), which
// is on the next day when it is not after `start`.
struct GigWindow {
    std::string date;
    int start = 0;
    int end = 0;

    bool operator==(const GigWindow&) const = default;
};

inline constexpr size_t kMaxGigWindows = 64;

// A layer's own settings in one deck, for the clips of that deck's columns; a field left unset is the layer's.
struct DeckLayer {
    std::optional<std::vector<Effect>> effects;
    std::optional<BlendMode> blend;
    std::optional<TransitionMode> transitionMode;
    std::optional<double> transitionBeats;
    std::optional<AutopilotMode> autopilotMode;
    std::optional<int> autopilotBars;

    bool operator==(const DeckLayer&) const = default;
};

// A group is a layer with a sub-stack: it renders its own layers, bottom to top, into one picture and then blends that
// picture as a layer does. A group has no clips, and groups do not nest. An output-only group reaches only the
// slices that take it as input, never the composition picture.
struct Layer {
    uint64_t id = 0;
    std::string name;
    double opacity = 1.0;
    // Applied in order to the layer's source.
    std::vector<Effect> effects;
    BlendMode blend = BlendMode::Normal;
    bool bypass = false;
    bool solo = false;
    bool group = false;
    bool outputOnly = false;
    // At a render scale below 100 %, this layer and those above it draw at output size, as a text layer does.
    bool fullResolution = false;
    std::vector<Layer> layers;
    // The layer's cells by column id; a column with no entry is an empty cell.
    std::map<uint64_t, ClipRef> clips;
    // The cell triggered last. An empty cell clears the layer.
    std::optional<Play> playing;
    // Columns whose clips are preloaded besides those of the active and next column.
    std::set<uint64_t> armed;
    Autopilot autopilot;
    Transition transition;
    // The ISF transitions' parameter values by transition name, then parameter; one left out is the input's default.
    // They follow the mode whichever deck picks it.
    std::map<std::string, std::map<std::string, double>> transitionParams{};
    // By deck id. Opacity, bypass and solo are never per deck.
    std::map<uint64_t, DeckLayer> decks{};
};

// The layer's chain, blend, transition and Autopilot as the clips of `deck` play them.
const std::vector<Effect>& deckEffects(const Layer& layer, uint64_t deck);
BlendMode deckBlend(const Layer& layer, uint64_t deck);
Transition deckTransition(const Layer& layer, uint64_t deck);
Autopilot deckAutopilot(const Layer& layer, uint64_t deck);

// The clip in the cell the layer plays, or null.
const ClipRef* playingClip(const Layer& layer);

struct Column {
    uint64_t id = 0;
    std::string name;

    bool operator==(const Column&) const = default;
};

// A page of columns. Column ids are unique across decks. Showing a deck with a palette selects that palette.
struct Deck {
    uint64_t id = 0;
    std::string name;
    std::vector<Column> columns;
    uint64_t palette = 0;

    bool operator==(const Deck&) const = default;
};

struct Composition;

// The deck holding the column, or 0.
uint64_t columnDeck(const Composition& composition, uint64_t column);

// A region of an input picture placed onto a screen. The input rect is x, y, width, height in the input's UV; the
// output corners are TL, TR, BR, BL as x, y pairs in the screen's 0..1 space, y down.
struct Slice {
    uint64_t id = 0;
    std::string name;
    // 0 is the composition, otherwise a layer or group id.
    uint64_t input = 0;
    std::array<double, 4> inputRect{0.0, 0.0, 1.0, 1.0};
    std::array<double, 8> corners{0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0};
    Warp warp;
    SoftEdge softEdge;
    ColorAdjust color;
    std::vector<Mask> masks;

    bool operator==(const Slice&) const = default;
};

// One destination surface, sent to the display with a stable identity (never an index), so it finds its display
// again after a reconnect. An empty display means the screen has no output.
struct Screen {
    uint64_t id = 0;
    std::string name;
    std::string display;
    // Shows the composition-space test card through its slices instead of their inputs.
    bool testCard = false;
    std::vector<Slice> slices;
    ColorAdjust color;
    std::vector<Mask> masks;
    // The frame's top-left on the Map canvas, in board units; none lays it out in the Map's grid.
    std::optional<std::array<double, 2>> mapPosition{};
    // Left off the Map canvas; its output runs on.
    bool hidden = false;

    bool operator==(const Screen&) const = default;
};

// Keeps a screen's slice uniforms within one vertex-bytes upload.
inline constexpr size_t kMaxSlicesPerScreen = 64;

inline constexpr double kMinBpm = 20.0;
inline constexpr double kMaxBpm = 500.0;

// A named colour of a palette, straight RGBA, 0..1. Its id is unique within its palette; links name it.
struct PaletteColour {
    std::string name;
    std::array<double, 4> colour{1.0, 1.0, 1.0, 1.0};
    uint64_t id = 0;

    bool operator==(const PaletteColour&) const = default;
};

// A named list of colours saved with the show, which every colour picker offers. Palettes match by slot: a colour
// linked to a palette colour takes the colour in the same slot of the active palette.
struct ColourPalette {
    uint64_t id = 0;
    std::string name;
    std::vector<PaletteColour> colours;

    bool operator==(const ColourPalette&) const = default;
};

inline constexpr size_t kMaxPalettes = 16;
inline constexpr size_t kMaxPaletteColours = 64;
// Names the palette where an effect id goes: in a Cue's effect entries and the Autopilot state, the palettes are the
// steps of an effect with this id.
inline constexpr uint64_t kPaletteEffect = 0;

// Layers bottom to top.
struct Composition {
    std::vector<Layer> layers;
    double bpm = 120.0;
    std::vector<Deck> decks{{1, "Deck 1", {{1, "Column 1"}, {2, "Column 2"}, {3, "Column 3"}, {4, "Column 4"}}}};
    // The deck the clip grid shows.
    uint64_t deck = 1;
    // The column triggered last, 0 before the first.
    uint64_t column = 0;
    // Triggers land on the next multiple of this many beats on the beat clock; 0 lands them at once.
    int quantise = 4;
    std::vector<Cue> cues{};
    Timeline timeline{};
    // By date, so the venue knows offline when a show runs.
    std::vector<GigWindow> gigs{};
    std::vector<Screen> screens{};
    // The test card on every screen, whatever each screen's own setting.
    bool testCard = false;
    std::vector<Guide> guides{};
    std::vector<Macro> macros{};
    std::vector<Modulator> modulators{};
    // Applied in order to the final picture before the outputs.
    std::vector<Effect> effects{};
    // How hard each source drives what it modulates, by source (kick, snare, hat, bass, lowBand, midBand, highBand,
    // lfo): its 0..1 value is scaled by this and clamped. A missing source is 1.
    std::map<std::string, double> sensitivity{};
    // The Low/Mid and Mid/High edges of the adjustable audio bands, in Hz.
    std::array<double, 2> bandEdges = kDefaultBandEdges;
    // The master opacity of the final picture, over black; a layer-0 opacity edit or modulation sets it.
    double opacity = 1.0;
    // Scales every Autopilot step and hold, in whole bars of at least one: 0.5, 1, 2, 4 or 8.
    double pace = 1.0;
    // Autopilot of whole columns: every `bars` bars it triggers the next or a random other column of the shown deck on
    // every layer, as a column trigger does, while the layers' own Autopilots pause. A trigger holds it for `hold` bars.
    Autopilot columnAutopilot{};
    // The frame budget guard may draw layers at half resolution when frames overrun.
    bool frameGuard = true;
    std::vector<ColourPalette> palettes{};
    // The active palette by id; 0 or a palette that is gone is the first.
    uint64_t palette = 0;
    // Steps through the palettes as an effect's Autopilot steps through its steps, crossfading every linked colour
    // over `paletteTransition` beats; `palettePlaying` is the palette selected last, by index.
    Autopilot palettePilot{};
    double paletteTransition = kDefaultStepTransition;
    std::optional<StepPlay> palettePlaying{};
};

// Every effect on `layer` (0 is the composition): its own chain, then its clips' chains in column order.
std::vector<const Effect*> effectsOn(const Composition& composition, uint64_t layer);
// The effect with id `effect` anywhere in the composition, or null; `layer` receives the layer whose chain or clip
// holds it (0 is the composition).
const Effect* effectById(const Composition& composition, uint64_t effect, uint64_t* layer = nullptr);

// Every layer of the composition, each group before its own layers.
template <typename Layers, typename F>
void forEachLayer(Layers& layers, F&& f)
{
    for (auto& layer : layers) {
        f(layer);
        forEachLayer(layer.layers, f);
    }
}

// Every effect chain with the layer holding it and, for a clip's, its column: the composition's, then each layer's
// own, its decks' and its clips' in column order.
template <typename C, typename F>
void forEachChain(C& composition, F&& f)
{
    f(composition.effects, uint64_t(0), std::optional<uint64_t>());
    forEachLayer(composition.layers, [&](auto& layer) {
        f(layer.effects, layer.id, std::optional<uint64_t>());
        for (auto& [deck, settings] : layer.decks)
            if (settings.effects)
                f(*settings.effects, layer.id, std::optional<uint64_t>());
        for (auto& [column, clip] : layer.clips)
            f(clip.effects, layer.id, std::optional<uint64_t>(column));
    });
}

Composition defaultComposition();

// Layer 0 is the composition's master opacity.
struct SetLayerOpacity {
    uint64_t layer = 0;
    double opacity = 1.0;
};

// Puts a clip into the cell the layer plays and starts it now (into the shown deck's first column when the layer plays
// nothing). A null clip empties that cell, which clears the layer. The clip already there changes only its transport.
struct SetLayerClip {
    uint64_t layer = 0;
    std::optional<ClipRef> clip;
};

// Sets or empties one cell. A new clip in the cell a layer plays starts at once.
struct SetSlot {
    uint64_t layer = 0;
    uint64_t column = 0;
    std::optional<ClipRef> clip;
};

struct Cell {
    uint64_t layer = 0;
    uint64_t column = 0;
    ClipRef clip;
};

// Adds a column to `deck`, at the end without an index; a zero id gets a fresh one. `cells` fill it, whatever column
// they name.
struct AddColumn {
    uint64_t deck = 0;
    std::optional<size_t> index;
    Column column;
    std::vector<Cell> cells;
};

// Removes a column with its cells. Layers playing it clear.
struct RemoveColumn {
    uint64_t column = 0;
};

// Adds a deck, at the end without an index; zero ids get fresh ones, and `cells` fill its columns. `layers` are the
// layers' own settings in it, by layer id.
struct AddDeck {
    std::optional<size_t> index;
    Deck deck;
    std::vector<Cell> cells;
    std::map<uint64_t, DeckLayer> layers{};
};

// Removes a deck with its columns and their cells. The last deck stays.
struct RemoveDeck {
    uint64_t deck = 0;
};

// Reorders a deck: `index` is its position afterwards. The shown deck stays shown.
struct MoveDeck {
    uint64_t deck = 0;
    size_t index = 0;
};

// 0 (off), 1 (a beat), 2 (half a bar) or whole bars of 4 beats, up to 16 bars.
struct SetQuantise {
    int beats = 4;
};

// Live performance actions, applied as changes but never undone (see TriggerService). `beat` is when the trigger was
// given; applying it moves the beat onto the composition's quantisation.
struct TriggerClip {
    uint64_t layer = 0;
    uint64_t column = 0;
    double beat = 0.0;
};

// Every layer plays its cell in the column; a layer whose cell there is empty clears, as in Resolume.
struct TriggerColumn {
    uint64_t column = 0;
    double beat = 0.0;
};

// Live: the layer plays nothing until its next trigger.
struct StopLayer {
    uint64_t layer = 0;
};

// Shows another deck; what plays keeps playing. A deck with a palette selects it at `beat`.
struct SelectDeck {
    uint64_t deck = 0;
    std::optional<double> beat{};
};

struct ArmClip {
    uint64_t layer = 0;
    uint64_t column = 0;
    bool armed = true;
};

// The beat clock's tempo.
struct SetTempo {
    double bpm = 120.0;
};

// Changes only the fields that are set, of the clip in `column`, or without one in the cell the layer plays. Applying
// it fills in the column.
struct SetClipTransport {
    uint64_t layer = 0;
    std::optional<SyncMode> sync;
    std::optional<PlayMode> playMode;
    std::optional<int> beats;
    std::optional<double> jumpInterval{};
    std::optional<double> jumpDistance{};
    std::optional<uint64_t> column{};
    std::optional<double> speed{};
    std::optional<double> in{};
    std::optional<double> out{};
};

// The quantisation triggers of the clip in the cell land on (as SetQuantise); none keeps the composition's.
struct SetClipTrigger {
    uint64_t layer = 0;
    uint64_t column = 0;
    std::optional<int> quantise;
};

// Changes only the fields that are set. Bars are 1..64, the hold 0..64.
struct SetLayerAutopilot {
    uint64_t layer = 0;
    std::optional<AutopilotMode> mode;
    std::optional<int> bars;
    std::optional<int> hold{};
};

// The composition's Autopilot pace: 0.5, 1, 2, 4 or 8.
struct SetAutopilotPace {
    double pace = 1.0;
};

// The composition's column Autopilot; changes only the fields that are set. Bars are 1..64, the hold 0..64.
struct SetColumnAutopilot {
    std::optional<AutopilotMode> mode;
    std::optional<int> bars;
    std::optional<int> hold{};
};

struct SetFrameGuard {
    bool on = true;
};

// The bars the clip in the cell plays under Autopilot, 0..64; 0 keeps the layer's step.
struct SetClipStep {
    uint64_t layer = 0;
    uint64_t column = 0;
    int bars = 0;
};

// Changes only the fields that are set. Beats are 0..64; 0 cuts.
struct SetLayerTransition {
    uint64_t layer = 0;
    std::optional<TransitionMode> mode;
    std::optional<double> beats;
};

// A parameter of the layer's ISF transition `mode`, within its input's range; none returns it to the default.
struct SetTransitionParam {
    uint64_t layer = 0;
    TransitionMode mode = TransitionMode::Alpha;
    std::string name;
    std::optional<double> value;
};

// The beats of the transition into the clip in the cell, 0..64; none keeps the layer's.
struct SetClipTransition {
    uint64_t layer = 0;
    uint64_t column = 0;
    std::optional<double> beats;
};

// Applying it completes the edit: a zero id gets a fresh one, a missing index means the end, missing params get their
// defaults. Layer 0 is the composition; with a column the effect goes onto the clip in that cell of the layer, with a
// deck onto the layer's own chain in that deck.
struct AddEffect {
    uint64_t layer = 0;
    std::optional<size_t> index;
    Effect effect;
    std::optional<uint64_t> column{};
    std::optional<uint64_t> deck{};
};

// The edits below find the effect by its id, in whichever chain holds it.

struct RemoveEffect {
    uint64_t effect = 0;
};

// Reorders the effect within its chain or, with a layer, moves it into the chain AddEffect would add to there, keeping
// its id and so its links and running state.
struct MoveEffect {
    uint64_t effect = 0;
    size_t index = 0;
    std::optional<uint64_t> layer{};
    std::optional<uint64_t> column{};
    std::optional<uint64_t> deck{};
};

struct SetEffectParam {
    uint64_t effect = 0;
    std::string param;
    double value = 0.0;
};

// Several parameters of one effect at once, e.g. a preset applied; all of them apply or none. With `preset` it also
// names the preset the effect is set from (empty: none).
struct SetEffectParams {
    uint64_t effect = 0;
    std::map<std::string, double> params;
    std::optional<std::string> preset{};
};

struct SetEffectBypass {
    uint64_t effect = 0;
    bool bypass = false;
};

// Replaces the effect's steps; the parameters they name must be the effect's.
struct SetEffectSteps {
    uint64_t effect = 0;
    std::vector<EffectStep> steps;
};

// Changes only the fields that are set: the effect Autopilot's mode, bars 1..64 and hold 0..64, and the transition
// into a step, 0..64 beats.
struct SetEffectAutopilot {
    uint64_t effect = 0;
    std::optional<AutopilotMode> mode;
    std::optional<int> bars;
    std::optional<int> hold{};
    std::optional<double> transition{};
};

// Live: the effect moves to one of its steps at `beat`; applying it moves the beat onto the composition's quantisation.
struct TriggerStep {
    uint64_t effect = 0;
    size_t step = 0;
    double beat = 0.0;
};

// A source's sensitivity, 0..4; 1 is as the source delivers.
struct SetSensitivity {
    std::string source;
    double value = 1.0;
};

inline constexpr double kMaxSensitivity = 4.0;

// Moves the adjustable audio bands' edges; the analyser glides there, so a band's level never jumps.
struct SetBandEdges {
    std::array<double, 2> edges = kDefaultBandEdges;
};

// Replaces the layer's own settings in the deck; empty settings make every field the layer's again. Applying it
// completes the chain as AddEffect does.
struct SetDeckLayer {
    uint64_t layer = 0;
    uint64_t deck = 0;
    DeckLayer settings;
};

struct SetLayerBlend {
    uint64_t layer = 0;
    BlendMode blend = BlendMode::Normal;
};

struct SetLayerBypass {
    uint64_t layer = 0;
    bool bypass = false;
};

struct SetLayerSolo {
    uint64_t layer = 0;
    bool solo = false;
};

struct SetLayerFullResolution {
    uint64_t layer = 0;
    bool fullResolution = false;
};

// Adds a layer, or a group with `layer.group`, into `parent` (0 is the composition). Applying it completes the edit:
// zero ids get fresh ones, a missing index means the top, missing effect params get their defaults.
struct AddLayer {
    uint64_t parent = 0;
    std::optional<size_t> index;
    Layer layer;
};

struct RemoveLayer {
    uint64_t layer = 0;
};

// Reorders a layer and moves it into or out of a group: `index` is its position within `parent` afterwards.
struct MoveLayer {
    uint64_t layer = 0;
    uint64_t parent = 0;
    size_t index = 0;
};

struct SetGroupOutputOnly {
    uint64_t layer = 0;
    bool outputOnly = false;
};

// Gives a display to a group: a new Screen on that display with one full slice taking the group as input. Applying it
// completes the edit with the screen it created.
struct AssignDisplay {
    uint64_t group = 0;
    std::string display;
    std::optional<Screen> screen{};
};

// Zero ids get fresh ones; a missing index means the end.
struct AddScreen {
    std::optional<size_t> index;
    Screen screen;
};

struct RemoveScreen {
    uint64_t screen = 0;
};

// An empty display detaches the screen from any output.
struct SetScreenDisplay {
    uint64_t screen = 0;
    std::string display;
};

// Screen 0 is the composition's test card on every screen.
struct SetScreenTestCard {
    uint64_t screen = 0;
    bool testCard = false;
};

// None returns the screen to the Map's grid.
struct SetScreenPosition {
    uint64_t screen = 0;
    std::optional<std::array<double, 2>> position;
};

struct SetScreenHidden {
    uint64_t screen = 0;
    bool hidden = false;
};

// Reorders a screen: `index` is its position afterwards.
struct MoveScreen {
    uint64_t screen = 0;
    size_t index = 0;
};

struct AddSlice {
    uint64_t screen = 0;
    std::optional<size_t> index;
    Slice slice;
};

struct RemoveSlice {
    uint64_t slice = 0;
};

struct SetSliceCorners {
    uint64_t slice = 0;
    std::array<double, 8> corners{};
};

// Changes only the fields that are set.
struct SetSliceInput {
    uint64_t slice = 0;
    std::optional<uint64_t> input;
    std::optional<std::array<double, 4>> inputRect;
};

// Cue edits. Applying AddCue completes it: a zero id gets a fresh one, a missing index means the end.
struct AddCue {
    std::optional<size_t> index;
    Cue cue;
};

struct RemoveCue {
    uint64_t cue = 0;
};

// Replaces the Cue with the same id.
struct SetCue {
    Cue cue;
};

struct SetCueEnabled {
    uint64_t cue = 0;
    bool enabled = true;
};

// No `every` fires the Cue once.
struct SetCueRepeat {
    uint64_t cue = 0;
    std::optional<int> every;
};

// Sets the Gig window of `date`, or without a window removes it.
struct SetGigWindow {
    std::string date;
    std::optional<GigWindow> window{};
};

// Live: replaces the timeline.
struct SetTimeline {
    Timeline timeline;
};

// Live: takes back triggers still waiting at `beat`. Each listed layer plays `playing` again, the cell it shows; a
// layer whose trigger has landed by then is left alone.
struct CancelTrigger {
    struct Restore {
        uint64_t layer = 0;
        std::optional<Play> playing;
    };
    double beat = 0.0;
    std::vector<Restore> layers;
};

// Names a layer or group, a deck, a column, a macro, a screen, a slice or a mask (`kind` "layer", "deck", "column",
// "macro", "screen", "slice" or "mask").
struct Rename {
    std::string kind;
    uint64_t id = 0;
    std::string name;
};

// Replaces the palette with the same id, or adds it (at `index`, else at the end); a zero id gets a fresh one.
struct SetPalette {
    ColourPalette palette;
    std::optional<size_t> index{};
};

struct RemovePalette {
    uint64_t palette = 0;
};

// Live: makes the palette active, recolouring every linked colour; the engine crossfades to it from `beat`, which
// applying it moves onto the composition's quantisation.
struct SelectPalette {
    uint64_t palette = 0;
    double beat = 0.0;
};

// Changes only the fields that are set of the palette Autopilot, as SetEffectAutopilot does an effect's.
struct SetPaletteAutopilot {
    std::optional<AutopilotMode> mode;
    std::optional<int> bars;
    std::optional<int> hold{};
    std::optional<double> transition{};
};

// The palette showing the deck selects; 0 for none.
struct SetDeckPalette {
    uint64_t deck = 0;
    uint64_t palette = 0;
};

inline constexpr const char* kTextColour = "color";
inline constexpr const char* kTextOutline = "outlineColor";
inline constexpr const char* kPaletteSlot = "slot";

// A colour in the document: an effect's colour group (`effect` and the group's key), the fill (kTextColour) or
// outline (kTextOutline) colour of the text clip in a cell (`layer`, `column`), or a palette's colour (kPaletteSlot,
// `palette` and `slot`).
struct ColourRef {
    uint64_t effect = 0;
    std::string group;
    uint64_t layer = 0;
    uint64_t column = 0;
    uint64_t palette = 0;
    size_t slot = 0;

    auto operator<=>(const ColourRef&) const = default;
};

// Links an effect's colour group or a text colour to the active palette's colour with id `link`, giving it that
// colour now and whenever the palette colour changes; none unlinks it, keeping its colour.
struct SetPaletteLink {
    ColourRef colour;
    std::optional<uint64_t> link;
};

// Replaces the whole document, as loading a composition file does, in one undo step.
struct LoadComposition {
    Composition composition;
};

struct Batch;

// Everything that changes the document. Undo/redo are not edits: they reapply recorded ones.
using Edit = std::variant<SetLayerOpacity, SetLayerClip, SetTempo, SetClipTransport, AddEffect, RemoveEffect,
                          MoveEffect, SetEffectParam, SetLayerBlend, SetLayerBypass, SetLayerSolo, AddLayer,
                          RemoveLayer, MoveLayer, SetEffectParams, SetSlot, AddColumn, RemoveColumn, AddDeck,
                          RemoveDeck, SetQuantise, TriggerClip, TriggerColumn, SelectDeck, ArmClip, SetGroupOutputOnly,
                          AssignDisplay, AddScreen, RemoveScreen, SetScreenDisplay, SetScreenTestCard, AddSlice,
                          RemoveSlice, SetSliceCorners, SetSliceInput, SetLayerAutopilot, SetClipStep,
                          SetLayerTransition, SetClipTransition, AddMacro, RemoveMacro, SetMacroValue,
                          AddMacroTarget, RemoveMacroTarget, AddModulator, RemoveModulator, SetModulator, SetMacros,
                          LoadComposition, SetSliceWarp, SetSoftEdges, SetSliceColor, SetScreenColor, AddMask,
                          RemoveMask, SetMask, SetGuides, AddCue, RemoveCue, SetCue, SetCueEnabled,
                          SetCueRepeat, SetGigWindow, SetTimeline,
                          CancelTrigger, SetClipText, SetMacroTarget, Rename,
                          SetEffectBypass, SetSensitivity, SetClipTrigger, StopLayer,
                          SetAutopilotPace, SetColumnAutopilot, SetFrameGuard, SetDeckLayer, MoveDeck, SetPalette,
                          RemovePalette, SetPaletteLink, SetScreenPosition, SetScreenHidden, MoveScreen,
                          SetEffectSteps, SetEffectAutopilot, TriggerStep, SelectPalette, SetPaletteAutopilot,
                          SetDeckPalette, SetTransitionParam, SetBandEdges, SetLayerFullResolution, Batch>;

// Several edits applied in order as one change and one undo step: all of them, or none when one does not apply. Live
// edits cannot be part of it.
struct Batch {
    std::vector<Edit> edits;
};

// Live edits are never undone, so they are not accepted as commands.
bool isLive(const Edit& edit);

// A malformed message or an edit that does not fit the document.
struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Throws ProtocolError unless an Autopilot's bars are within 1..kMaxStepBars and its hold within 0..kMaxStepBars.
void checkAutopilotBars(std::optional<int> bars, std::optional<int> hold);

// Applies `edit`, completing it in place, and returns the edit that reverts it (a live edit returns itself). Throws
// ProtocolError and leaves the document untouched when the edit does not apply.
Edit applyEdit(Composition& composition, Edit& edit);

// An edit is a flat object: {"op": ..., fields}. Extra keys are ignored, so commands and changes carry it inline.
nlohmann::json editToJson(const Edit& edit);
Edit editFromJson(const nlohmann::json& j);

void to_json(nlohmann::json& j, const Transport& transport);
void from_json(const nlohmann::json& j, Transport& transport);
void to_json(nlohmann::json& j, const ClipRef& clip);
void from_json(const nlohmann::json& j, ClipRef& clip);
void to_json(nlohmann::json& j, const EffectStep& step);
void from_json(const nlohmann::json& j, EffectStep& step);
void to_json(nlohmann::json& j, const Effect& effect);
void from_json(const nlohmann::json& j, Effect& effect);
void to_json(nlohmann::json& j, const Deck& deck);
void from_json(const nlohmann::json& j, Deck& deck);
void to_json(nlohmann::json& j, const DeckLayer& settings);
void from_json(const nlohmann::json& j, DeckLayer& settings);
void to_json(nlohmann::json& j, const Layer& layer);
void from_json(const nlohmann::json& j, Layer& layer);
void to_json(nlohmann::json& j, const Slice& slice);
void from_json(const nlohmann::json& j, Slice& slice);
void to_json(nlohmann::json& j, const Screen& screen);
void from_json(const nlohmann::json& j, Screen& screen);
void to_json(nlohmann::json& j, const Composition& composition);
void from_json(const nlohmann::json& j, Composition& composition);

} // namespace azl
