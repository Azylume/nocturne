#pragma once

#include "document/palette.hpp"
#include "ui/app/params.hpp"
#include "ui/client/connection.hpp"
#include "ui/client/director_media.hpp"
#include "ui/client/engine_client.hpp"
#include "ui/client/local_previews.hpp"
#include "ui/client/return_audio_player.hpp"
#include "venues/venue_monitor.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace azl::ui {

class MidiControl;

// The UI's side of one engine, on the message thread: the client's state as JUCE images and a cached composition,
// plus what only this UI knows (selection, which monitor shows what). Views register to hear what changed.
class Session : private EngineClient::Listener, private juce::AsyncUpdater, private juce::Timer {
public:
    enum Change : int {
        Document = 1,
        Clock = 2,
        Library = 4,
        Stills = 8,
        Previews = 16,
        Connection = 32,
        Selection = 64,
        Status = 128,
        Modulation = 256,
        Audio = 512,
        MacroSets = 1024,
        // Learn mode, the selected macro, open Animate boxes, collapsed cards, a parameter jumped to.
        Panels = 2048,
        // The engine machine's displays.
        Outputs = 4096,
        Record = 8192,
        Shows = 16384,
        Presets = 32768,
        // The venue list or a venue's dot.
        Venues = 65536,
        // The engine machine's health, while the Machines tab watches it.
        Machine = 131072,
        // This machine's media: what is local, being fetched or chosen to keep.
        Media = 262144,
        // The engine machine's performance and cache settings.
        Settings = 524288,
        // A new picture of an output's return video, while the Machines tab watches it.
        ReturnVideo = 1048576,
        // The night's deck switches.
        Switches = 2097152,
    };

    struct View {
        virtual ~View() = default;
        virtual void sessionChanged(int changes) = 0;
    };

    struct ClipCell {
        uint64_t layer = 0;
        uint64_t column = 0;
        auto operator<=>(const ClipCell&) const = default;
    };

    // Where previews are rendered: Auto renders them on this machine when the engine runs on another one (On also for
    // an engine on this machine), so the engine renders none for this UI except of media this machine lacks.
    enum class LocalRender { Auto, Off, On };

    // `media` without a library takes the local previews' one when they run.
    Session(std::string host, uint16_t port, bool surfaces = true, LocalRender localRender = LocalRender::Auto,
            LocalPreviews::Options localOptions = {}, DirectorMedia::Options media = {});
    ~Session() override;

    EngineClient& client() { return client_; }
    // Null while the engine renders this UI's previews.
    LocalPreviews* localPreviews() { return local_.get(); }
    DirectorMedia& media() { return *media_; }
    const std::string& host() const { return host_; }
    uint16_t port() const { return port_; }
    // Why the engine cannot be reached, e.g. its name does not resolve; empty once connected.
    const std::string& connectionError() const { return connectionError_; }
    // Hearing the engine machine's audio input here; a switch to another engine ends it.
    void setListening(bool on);
    bool listening() const { return client_.listening(); }
    ReturnAudioPlayer& returnAudio() { return returnAudio_; }

    // Leaves the engine and connects the whole UI to the one at host:port, showing nothing of the old one meanwhile;
    // the host is remembered for the next start. The old engine notices only a client leaving.
    void connectTo(const std::string& host, uint16_t port);
    // Keeps the venue list current from these sources (the system's in an interactive run); without it there is no
    // list, only typed hosts.
    void startVenues(VenueSources sources);
    VenueMonitor* venues() { return venues_.get(); }
    // A host typed in the venue menu ("host" or "host:port"): connects to it and keeps it in the list, also after a
    // restart.
    void connectToTyped(const std::string& typed);
    // The pairing token the UI presents to the engine machine `host` (an engine on another machine lets in only a
    // client with its token), kept per host in the settings; empty when none was given. Thread-safe.
    std::string tokenFor(const std::string& host) const;
    // Keeps `token` for `host`; the connection to it, if it is the one in view, starts again with it.
    void rememberToken(const std::string& host, const std::string& token);
    // The venue list's entry of the engine this UI talks to, if it is listed.
    std::optional<VenueEntry> currentVenue() const;

    void addView(View* view);
    void removeView(View* view);

    // As of the last Document change.
    const Composition& composition() const { return *composition_; }
    const Layer* layer(uint64_t id) const;
    const Deck* shownDeck() const;
    // A null image until the still arrives, or when the clip has none. Media this machine has gives its own still; of
    // the rest the engine sends one each, kept by hash.
    juce::Image still(const std::string& hash);

    // Keep the clip's media on this machine, or not; nothing follows its shows. Fetches or removes it at once.
    void keepClip(const std::string& hash, std::optional<bool> keep);
    // The show the engine has open, as KeepLocal names it; empty for a show never saved.
    std::string currentShow() const;
    // Keep every clip of the show the engine has open on this machine, or not.
    void keepShow(bool keep);
    juce::Image preview(const std::string& source) const;
    // The selected clip's moving preview while it is not playing; null until a picture arrives, and for other clips.
    juce::Image clipPreview(const std::string& hash) const { return preview("clip:" + hash); }

    std::optional<uint64_t> selectedLayer() const { return selectedLayer_; }
    std::optional<ClipCell> selectedClip() const { return selectedClip_; }
    void selectLayer(uint64_t layer);
    void selectClip(ClipCell cell);
    void selectComposition();

    // Every selected clip: the selected clip and those added by Cmd- or Shift-click, Cmd-A or a whole row. Only cells
    // holding a clip; UI-local, never sent to the engine.
    const std::set<ClipCell>& selectedClips() const { return selection_; }
    bool isSelected(ClipCell cell) const { return selection_.contains(cell); }
    // Cmd-click: adds the cell's clip, or takes it out.
    void toggleClip(ClipCell cell);
    // Shift-click: every clip in the rectangle from the last clicked cell to this one, in the deck in view.
    void selectRange(ClipCell cell);
    // Selects these cells, the first as the selected clip; a cell that holds no clip by the next change drops out.
    void selectClips(const std::vector<ClipCell>& cells);
    // Esc: keeps the layer selected but no clip.
    void clearClipSelection();
    // The layers that hold clips (not groups), top first, as the grid shows them.
    std::vector<uint64_t> gridLayers() const;
    // Every clip of the deck in view, or of one layer or one column of it.
    std::vector<ClipCell> deckClips(std::optional<uint64_t> layer = {}, std::optional<uint64_t> column = {}) const;
    const ClipRef* clip(ClipCell cell) const;
    // Where the cell sits in the grid of the deck in view: its row (top first) and column; none when not there.
    std::optional<std::pair<int, int>> gridPosition(ClipCell cell) const;
    // The cell `down` rows and `right` columns away in the grid of the deck in view; none past its edges.
    std::optional<ClipCell> offsetCell(ClipCell cell, int down, int right) const;
    // UI-local clipboards: clips by rows and columns from where the block is placed, and effects.
    struct CopiedClip {
        int row = 0;
        int column = 0;
        ClipRef clip;
    };
    std::vector<CopiedClip> copiedClips;
    std::vector<Effect> copiedEffects;
    // The clips an edit of `cell` acts on: every selected clip when the cell is one of them, else the cell alone.
    std::vector<ClipCell> clipTargets(ClipCell cell) const;
    // Sends the edits as one undo step: a single edit as it is, several as a Batch.
    void edit(std::vector<Edit> edits);
    // A clip-level edit by effect type and parameter name, so it fits any clip: the edits for one clip, none where it
    // does not fit (the clip has no effect of that type).
    struct ClipEdit {
        juce::String name;
        std::function<std::vector<Edit>(ClipCell, const ClipRef&)> edits;
    };
    // Applies the edit to every clip of clipTargets(cell) as one undo step, and remembers it for repeating.
    void editClips(ClipCell cell, const ClipEdit& edit);
    // Adds a copy of the effect with a fresh id; removes every effect of the type.
    static ClipEdit addingEffect(const Effect& effect);
    static ClipEdit removingEffects(const std::string& type);
    // Remembers an edit made on one clip some other way, for repeating.
    void rememberClipEdit(ClipEdit edit) { lastClipEdit_ = std::move(edit); }
    const std::optional<ClipEdit>& lastClipEdit() const { return lastClipEdit_; }
    // Cmd-R: the last clip-level edit on every selected clip as one undo step; the status counts the clips it does not
    // fit (they lack that effect or parameter).
    void repeatLastEdit();

    // "composition", "layer:N" for a group, or "screen:N".
    const std::string& monitorSource() const { return monitorSource_; }
    void setMonitorSource(const std::string& source);
    // The screen the Map view shows, 0 for none.
    void setMapScreen(uint64_t screen);
    uint64_t mapScreen() const { return mapScreen_; }
    // Previews the Map tab shows besides its screen: every screen's, and the input the Input view shows.
    void setMapPreviews(std::set<std::string> sources);

    // Off, nothing is watched, so the engine renders no previews for this UI; on by default.
    bool previewsOn() const { return previewsOn_; }
    void setPreviewsOn(bool on);

    // What the preview monitor shows: the selected clip's layer while it plays that clip, else the selected layer.
    std::string previewSource() const;

    // Puts clips into the layer's cells of the shown deck as one undo step: from `slot` on over what is there, or with
    // a negative slot into its empty cells. Past the deck's last column each clip adds a column.
    void placeClips(uint64_t layer, int slot, const std::vector<ClipRef>& clips);

    // Clicking a clip's thumbnail: a queued trigger is taken back, anything else triggers.
    void clickClip(ClipCell cell);
    // Esc: takes back every queued trigger.
    void cancelQueued();

    // The effect panel's tab: 0 the composition, 1 the selected layer, 2 the selected clip. Selecting switches it.
    int chainTab() const { return chainTab_; }
    void setChainTab(int tab);
    // The chain the effect panel edits, after its tab. The Clip tab with no clip to show has a column on no layer,
    // the Layer tab with no layer selected kNoLayer.
    ChainOwner chainOwner() const;
    // The Clip tab's clip: the selected clip, else the one the selected layer plays.
    std::optional<ClipCell> clipTabCell() const;

    // The value the engine renders: the base, or the driven value while macros or modulators move it.
    std::optional<double> drivenValue(const ParamRef& ref) const;
    const ModulationState& modulation() const { return modulation_; }
    // A clip's effect parameter sets the same parameter of the effect of its type on every selected clip.
    void setParam(const ParamRef& ref, double value);
    // The clip's effect parameter differs among the selected clips that have it.
    bool mixedParam(const ParamRef& ref) const;
    // A clip's colour sets the same colour of every selected clip that has it, as setParam does.
    void setColour(const ColourRef& ref, const Hsva& colour);
    // The colours picked last in this UI, newest first.
    const std::vector<Rgba>& recentColours() const { return recent_; }
    void addRecentColour(const Rgba& colour);
    static constexpr size_t kRecentColours = 12;

    int selectedMacro() const { return selectedMacro_; }
    void selectMacro(int slot);
    bool learning() const { return learning_; }
    void setLearning(bool on);
    // Adds the parameter to the macro in `slot`, creating the macros up to it if missing, as one undo step.
    void assignToMacro(int slot, const ParamRef& ref);
    void removeFromMacro(uint64_t macro, const ParamRef& ref);
    // Animates the parameter from "beat" or an audio feature, replacing the source of an animation it has.
    void animate(const ParamRef& ref, const std::string& source);
    void stopAnimating(const ParamRef& ref);
    bool animationOpen(const ParamRef& ref) const;
    void setAnimationOpen(const ParamRef& ref, bool open);
    bool collapsed(uint64_t effect) const { return collapsed_.contains(effect); }
    void setCollapsed(uint64_t effect, bool collapsed);
    // Whether the effect card's strip of steps is open; it starts closed.
    bool stepsOpen(uint64_t effect) const { return stepsOpen_.contains(effect); }
    void setStepsOpen(uint64_t effect, bool open);
    // Whether a preset group of the browser and the cards' Presets menus is open ("section/Classic", "effect/trails",
    // "group/trails/mine"); kept per machine in the settings once given, so it survives a restart.
    bool groupOpen(const std::string& key, bool byDefault) const;
    void setGroupOpen(const std::string& key, bool open);
    void useSettings(juce::PropertiesFile* settings);
    // MIDI controllers on this machine; nothing listens until it starts.
    MidiControl& midi() { return *midi_; }
    // Selects the chain that holds the parameter (showing the clip's deck) and opens its Animate box; panels bring it
    // into view and highlight it for a second.
    void goToParam(const ParamRef& ref);
    std::optional<ParamRef> focusedParam() const { return focused_; }

    // The engine's undo history: op names, oldest first.
    const std::vector<std::string>& pastEdits() const { return past_; }
    const std::vector<std::string>& futureEdits() const { return future_; }

    juce::String status() const { return status_; }
    bool statusIsError() const { return statusError_; }
    void showStatus(const juce::String& text, bool error);

private:
    void documentChanged() override { post(Document); }
    void clockChanged() override { post(Clock); }
    void libraryChanged() override { post(Library); }
    void stillArrived(const std::string&) override { post(Stills); }
    void previewArrived(const std::string&) override { post(Previews); }
    void connectionChanged() override { post(Connection); }
    void engineError(const std::string& message) override;
    void modulationChanged() override { post(Modulation); }
    void audioChanged() override { post(Audio); }
    void macroSetsChanged() override { post(MacroSets); }
    void presetsChanged() override { post(Presets); }
    void showsChanged() override { post(Shows); }
    void outputsChanged() override { post(Outputs); }
    void clipPreviewsChanged() override { post(Previews); }
    void recordChanged() override { post(Record); }
    void settingsChanged() override { post(Settings); }
    void machineChanged() override { post(Machine); }
    void switchesChanged() override { post(Switches); }
    void videoArrived() override { post(ReturnVideo); }
    void recordNotice(const std::string& notice) override { engineError(notice); }
    void outputsNotice(const std::string& notice) override { engineError(notice); }
    void prepareNotice(const std::string& notice) override { engineError(notice); }
    void compositionNotice(const std::string& notice) override { engineError(notice); }

    void post(int changes);
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void notify(int changes);
    std::set<std::string> sourcesToWatch() const;
    void connect();
    // Connects to the same engine again, with the token kept for it now.
    void reconnect();
    void startLocalPreviews(const LocalPreviews::Options& options);
    // The local follower can render the source: it shows no media this machine lacks.
    bool rendersLocally(const std::string& source) const;
    std::set<std::string> shownMedia(const std::string& source) const;
    EngineClient& clientFor(const std::string& source);
    void updateWatch();
    void mediaChanged(const std::string& hash);
    void reconcileMedia();
    std::set<std::string> clipToWatch() const;
    void updateClipWatch();
    void dropUnwatched();
    void refreshPreviews();

    std::string host_;
    uint16_t port_;
    std::string token_;
    mutable std::mutex tokensMutex_;
    std::map<std::string, std::string> tokens_;
    const bool surfaces_;
    const LocalRender localRender_;
    const LocalPreviews::Options localOptions_;
    ReturnAudioPlayer returnAudio_;
    EngineClient client_;
    std::unique_ptr<DirectorMedia> media_;
    std::mutex mediaMutex_;
    std::set<std::string> mediaArrived_;
    std::unique_ptr<LocalPreviews> local_;
    std::unique_ptr<azl::ui::Connection> connection_;
    std::unique_ptr<VenueMonitor> venues_;
    std::atomic<int> pending_{0};
    std::vector<View*> views_;
    std::shared_ptr<const Composition> composition_ = std::make_shared<const Composition>();
    std::map<std::string, juce::Image> stills_;
    struct Picture {
        uint64_t frame = UINT64_MAX;
        juce::Image image;
    };
    std::map<std::string, Picture> previews_;
    std::optional<uint64_t> selectedLayer_;
    std::optional<ClipCell> selectedClip_;
    std::set<ClipCell> selection_;
    std::optional<ClipEdit> lastClipEdit_;
    std::optional<ClipCell> anchor_;
    std::string monitorSource_ = "composition";
    uint64_t mapScreen_ = 0;
    std::set<std::string> mapPreviews_;
    bool previewsOn_ = true;
    std::set<std::string> watched_;
    std::set<std::string> watchedClips_;
    // The sources of both that the local follower renders.
    std::set<std::string> localSources_;
    std::set<std::string> localClips_;
    juce::String status_;
    bool statusError_ = false;
    std::string connectionError_;
    juce::String pendingError_;
    std::mutex errorMutex_;
    double statusUntil_ = 0.0;
    ModulationState modulation_;
    int ticks_ = 0;
    int chainTab_ = 0;
    int selectedMacro_ = 0;
    bool learning_ = false;
    std::set<std::tuple<uint64_t, uint64_t, std::string>> animOpen_;
    std::set<uint64_t> collapsed_;
    std::set<uint64_t> stepsOpen_;
    std::map<std::string, bool> groupsOpen_;
    juce::PropertiesFile* settings_ = nullptr;
    std::optional<ParamRef> focused_;
    double focusedUntil_ = 0.0;
    std::vector<std::string> past_, future_;
    std::vector<Rgba> recent_;
    void trackHistory(const History& history);
    // Last, so it goes first: it ends its undo step through the client.
    std::unique_ptr<MidiControl> midi_;
};

// A typed "host" or "host:port" (an IPv6 address takes the default port); the default port when none is given.
std::pair<std::string, uint16_t> hostAndPort(const std::string& typed);

// The engine host an interactive UI connects to: `given` (its --host), remembered in `settings` for next time, or with
// none given the one remembered there, else this machine.
std::string rememberedHost(juce::PropertiesFile& settings, const std::string& given);

} // namespace azl::ui
