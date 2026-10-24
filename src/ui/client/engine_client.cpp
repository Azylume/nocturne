#include "ui/client/engine_client.hpp"

#include "effects/effect_registry.hpp"
#include "effects/isf.hpp"
#include "protocol/build_info.hpp"
#include "protocol/bundle_push.hpp"
#include "protocol/prepare.hpp"
#include "protocol/preview_surface.hpp"

#include <IOSurface/IOSurface.h>

#include <cstring>
#include <filesystem>
#include <optional>

namespace azl::ui {

namespace {

// An engine that does not say its registry version is not judged.
std::string skewOf(const Message& snapshot)
{
    const std::string effects = snapshot.value("effects", "");
    if (effects.empty() || effects == effectRegistryVersion())
        return "";
    if (snapshot.value("build", "") > std::string(buildId()))
        return "UI older than the engine: update it";
    return "UI newer than the engine: update the engine";
}

const Layer* findLayer(const Composition& composition, uint64_t id)
{
    const Layer* found = nullptr;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        if (layer.id == id)
            found = &layer;
    });
    return found;
}

LibraryItem libraryItem(const Message& entry)
{
    return {entry.value("hash", ""), entry.value("name", ""), entry.value("path", ""), entry.value("status", "")};
}

std::vector<uint8_t> bytesOf(const Message& binary)
{
    const auto& data = binary.get_binary();
    return {data.begin(), data.end()};
}

} // namespace

bool isLocalHost(const std::string& host)
{
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

// The ring of shared surfaces the engine paints one source into.
struct EngineClient::Surfaces {
    std::vector<IOSurfaceRef> surfaces;

    ~Surfaces()
    {
        for (IOSurfaceRef surface : surfaces)
            if (surface)
                CFRelease(surface);
    }

    static std::optional<int64_t> frameIn(IOSurfaceRef surface)
    {
        CFTypeRef value = IOSurfaceCopyValue(surface, previewFrameKey());
        if (!value)
            return std::nullopt;
        int64_t frame = -1;
        if (CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &frame);
        CFRelease(value);
        return frame;
    }

    // The engine may have painted the surface again since it named it, or be painting it now: the picture is taken
    // only when the surface held one finished frame throughout the copy, and is of that frame.
    bool read(size_t index, PreviewPicture& picture) const
    {
        if (index >= surfaces.size() || !surfaces[index])
            return false;
        IOSurfaceRef surface = surfaces[index];
        const std::optional<int64_t> before = frameIn(surface);
        if (before && *before < 0)
            return false;
        picture.width = uint32_t(IOSurfaceGetWidth(surface));
        picture.height = uint32_t(IOSurfaceGetHeight(surface));
        const size_t row = size_t(picture.width) * 4;
        picture.bgra.resize(row * picture.height);
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
        const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
        const size_t stride = IOSurfaceGetBytesPerRow(surface);
        for (uint32_t y = 0; y < picture.height; ++y)
            std::memcpy(picture.bgra.data() + y * row, base + y * stride, row);
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
        if (!before)
            return true;
        if (frameIn(surface) != before)
            return false;
        picture.frame = uint64_t(*before);
        return true;
    }
};

EngineClient::EngineClient(Listener& listener) : listener_(listener) {}

EngineClient::~EngineClient()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    updateReplied_.notify_all();
    if (updater_.joinable())
        updater_.join();
}

void EngineClient::attached(Send send, bool local, const std::string& token)
{
    {
        std::lock_guard lock(mutex_);
        send_ = std::move(send);
        local_ = local;
        mirror_ = DocumentMirror();
        stillsAsked_.clear();
        for (const auto& [hash, jpeg] : stills_)
            stillsAsked_.insert(hash);
        sendLocked(msg::hello(token));
        sendLocked({{"type", "library"}, {"op", "list"}});
        sendLocked({{"type", "preset"}, {"op", "list"}});
        sendLocked({{"type", "isf"}, {"op", "list"}, {"text", true}});
        sendLocked({{"type", "modulation"}, {"op", "list"}});
        sendLocked({{"type", "audio"}, {"op", "subscribe"}, {"rate", 20}});
        sendLocked({{"type", "audio"}, {"op", "devices"}});
        sendLocked({{"type", "audio"}, {"op", "input"}});
        sendLocked({{"type", "audio"}, {"op", "settings"}});
        sendLocked({{"type", "outputs"}, {"op", "list"}});
        sendLocked({{"type", "record"}, {"op", "state"}});
        settings_ = {};
        sendLocked({{"type", "settings"}, {"op", "state"}});
        sendLocked({{"type", "preview"}, {"op", "clips"}});
        sendLocked({{"type", "switches"}, {"op", "list"}, {"watch", true}});
        sendWatchLocked();
        if (!watchedClips_.empty())
            sendClipWatchLocked();
        if (watchingMachine_)
            sendLocked({{"type", "machine"}, {"op", "subscribe"}, {"rate", kMachineRate}});
        if (watchingVideo_)
            sendLocked({{"type", "video"}, {"op", "watch"}, {"on", true}});
        if (listening_)
            sendLocked({{"type", "listen"}, {"op", "start"}});
    }
    listener_.connectionChanged();
}

void EngineClient::detached()
{
    {
        std::lock_guard lock(mutex_);
        send_ = nullptr;
        mirror_ = DocumentMirror();
        surfaces_.clear();
        previews_.clear();
        adding_.clear();
    }
    video_.clear();
    updateReplied_.notify_all();
    listener_.connectionChanged();
}

void EngineClient::forget()
{
    std::lock_guard lock(mutex_);
    listening_ = false;
    mirror_ = DocumentMirror();
    prepared_.reset();
    composition_ = std::make_shared<const Composition>();
    clock_ = {};
    triggeredSinceClock_.clear();
    library_.clear();
    stills_.clear();
    stillsAsked_.clear();
    previews_.clear();
    surfaces_.clear();
    adding_.clear();
    failed_.clear();
    skew_.clear();
    build_.clear();
    history_ = {};
    savedHistory_ = {};
    unsavedAtSnapshot_ = false;
    savedPath_.clear();
    audioDevices_.clear();
    audioInput_ = {};
    audioGain_ = {};
    modulation_ = {};
    audio_ = {};
    presets_ = Message::object();
    presetEntries_.clear();
    presetDefaults_.clear();
    chainPresets_ = Message::object();
    macroSets_.clear();
    macroSetPreview_.reset();
    shows_ = {};
    displays_.clear();
    unmirror_ = true;
    record_ = {};
    settings_ = {};
    machine_.reset();
    switches_ = {};
    clipPreviews_ = false;
}

void followHistory(History& h, size_t undo, size_t redo, const std::string& undoOp, const std::string& redoOp)
{
    if (undo == h.past.size() + 1 && redo == 0) {
        h.past.push_back(undoOp);
        h.future.clear();
    } else if (undo + 1 == h.past.size() && redo == h.future.size() + 1) {
        h.future.push_back(h.past.back());
        h.past.pop_back();
    } else if (undo == h.past.size() + 1 && redo + 1 == h.future.size()) {
        h.past.push_back(h.future.back());
        h.future.pop_back();
    } else if (undo != h.past.size() || redo != h.future.size()) {
        h.past.assign(undo, "");
        h.future.assign(redo, "");
    }
    if (!h.past.empty())
        h.past.back() = undoOp;
    if (!h.future.empty())
        h.future.back() = redoOp;
    h.undo = undo;
    h.redo = redo;
    h.undoOp = undoOp;
    h.redoOp = redoOp;
}

void EngineClient::receive(const Message& message)
{
    if (const auto type = message.find("type"); type != message.end() && *type == "video") {
        if (video_.receive(message))
            listener_.videoArrived();
        return;
    }
    std::string type;
    std::vector<std::function<void()>> notify;
    {
        std::lock_guard lock(mutex_);
        try {
            type = message.value("type", "");
            receiveLocked(type, message, notify);
        } catch (const std::exception& e) {
            const std::string text = "cannot follow the engine (" + type + "): " + e.what();
            notify.push_back([this, text] { listener_.engineError(text); });
            if (type == "change" && !mirror_.synced())
                sendLocked(msg::snapshotRequest());
        }
    }
    for (auto& call : notify)
        call();
}

// The snapshot's show file, as the engine keeps it across restarts; edits from here on follow the history.
void EngineClient::followFile(const Message& snapshot)
{
    const Message file = snapshot.value("file", Message::object());
    savedPath_ = file.value("path", "");
    savedHistory_ = history_;
    unsavedAtSnapshot_ = file.value("unsaved", false);
}

void EngineClient::receiveLocked(const std::string& type, const Message& message,
                                 std::vector<std::function<void()>>& notify)
{
    if (type == "snapshot") {
        skew_ = skewOf(message);
        build_ = message.value("build", "");
    }
    if (type == "snapshot" || type == "change") {
        if (auto request = mirror_.receive(message))
            sendLocked(*request);
        if (auto h = message.find("history"); h != message.end() && h->is_object())
            followHistory(history_, h->value("undo", size_t(0)), h->value("redo", size_t(0)), h->value("undoOp", ""),
                          h->value("redoOp", ""));
        if (auto steps = message.find("steps"); steps != message.end() && steps->is_object()) {
            history_.past = steps->value("undo", std::vector<std::string>{});
            history_.future = steps->value("redo", std::vector<std::string>{});
        }
        if (type == "snapshot")
            followFile(message);
        if (mirror_.synced()) {
            if (prepared_)
                prepared_->follow(mirror_.composition());
            auto next = std::make_shared<const Composition>(prepared_ ? prepared_->composition()
                                                                      : mirror_.composition());
            forEachLayer(next->layers, [&](const Layer& layer) {
                const Layer* before = findLayer(*composition_, layer.id);
                if (layer.playing && (!before || before->playing != layer.playing))
                    triggeredSinceClock_.insert(layer.id);
            });
            composition_ = std::move(next);
            notify.push_back([this] { listener_.documentChanged(); });
        }
        if (const std::string notice = message.value("notice", ""); !notice.empty())
            notify.push_back([this, notice] { listener_.compositionNotice(notice); });
    } else if (type == "clock") {
        if (message.value("op", "") == "state") {
            receiveClock(message);
            notify.push_back([this] { listener_.clockChanged(); });
        }
    } else if (type == "library") {
        receiveLibrary(message, notify);
    } else if (type == "preview") {
        receivePreview(message, notify);
    } else if (type == "modulation") {
        receiveModulation(message);
        const bool state = message.value("op", "") == "state";
        notify.push_back([this, state] { state ? listener_.modulationChanged() : listener_.macroSetsChanged(); });
    } else if (type == "composition" && message.value("op", "") == "shows") {
        ShowList list{message.value("folder", ""), message.value("showsFolder", ""),
                      message.value("recent", std::vector<std::string>{})};
        for (const auto& s : message.value("shows", Message::array()))
            list.shows.push_back({s.value("path", ""), s.value("name", ""), s.value("modified", 0.0),
                                  s.value("size", uint64_t(0)), s.value("mediaPresent", false)});
        shows_ = std::move(list);
        notify.push_back([this] { listener_.showsChanged(); });
    } else if (type == "composition") {
        savedPath_ = message.value("path", "");
        savedHistory_ = history_;
        unsavedAtSnapshot_ = false;
        // The loadComposition change follows the reply.
        if (message.value("op", "") == "load")
            savedHistory_ = {history_.undo + 1, 0, "loadComposition", ""};
        notify.push_back([this] { listener_.documentChanged(); });
        if (const std::string notice = message.value("notice", ""); !notice.empty())
            notify.push_back([this, notice] { listener_.compositionNotice(notice); });
    } else if (type == "audio" && message.value("op", "") == "devices") {
        audioDevices_.clear();
        for (const auto& d : message.value("devices", Message::array()))
            audioDevices_.push_back({d.value("uid", ""), d.value("name", ""), d.value("channels", 0)});
        notify.push_back([this] { listener_.audioChanged(); });
    } else if (type == "audio" && message.value("op", "") == "input") {
        audioInput_ = {message.value("uid", ""),          message.value("name", ""),
                       message.value("channels", std::vector<int>{}), message.value("requested", ""),
                       message.value("state", ""),        message.value("permission", ""),
                       message.value("sampleRate", 0.0)};
        notify.push_back([this] { listener_.audioChanged(); });
    } else if (type == "audio" && message.value("op", "") == "settings") {
        audioGain_ = {message.value("autoGain", true), message.value("gainDb", 0.0)};
        notify.push_back([this] { listener_.audioChanged(); });
    } else if (type == "audio") {
        if (message.value("op", "") == "state") {
            receiveAudio(message);
            notify.push_back([this] { listener_.audioChanged(); });
        }
    } else if (type == "record") {
        receiveRecord(message, notify);
    } else if (type == "settings") {
        receiveSettings(message);
        notify.push_back([this] { listener_.settingsChanged(); });
    } else if (type == "isf") {
        receiveIsf(message, notify);
    } else if (type == "preset") {
        const std::string op = message.value("op", "");
        if (op == "list") {
            receivePresets(message);
            notify.push_back([this] { listener_.presetsChanged(); });
        } else if (op == "save" || op == "import" || op == "rename" || op == "delete" || op == "default" ||
                   op == "saveChain" || op == "renameChain" || op == "deleteChain" || op == "merge") {
            sendLocked({{"type", "preset"}, {"op", "list"}});
        }
    } else if (type == "outputs") {
        const std::string op = message.value("op", "");
        if (op == "list") {
            displays_.clear();
            for (const auto& d : message.value("displays", Message::array()))
                displays_.push_back({d.value("id", ""), d.value("name", ""), d.value("width", 0), d.value("height", 0),
                                     d.value("mirrorOf", "")});
            unmirror_ = message.value("unmirror", true);
            notify.push_back([this] { listener_.outputsChanged(); });
        } else if (op == "notice") {
            notify.push_back([this, text = message.value("text", "")] { listener_.outputsNotice(text); });
        }
    } else if (type == "listen") {
        if (listenSink_)
            notify.push_back([sink = listenSink_, message] { sink(message); });
    } else if ((type == "machine" || type == "error") && message.value("op", "") == "update") {
        updateReply_ = message;
        updateReplied_.notify_all();
    } else if (type == "machine") {
        const std::string op = message.value("op", "");
        if (op == "status")
            receiveMachine(message);
        else if (machine_ && op == "lock")
            machine_->showLock = message.value("on", false);
        else if (machine_ && op == "testcard")
            machine_->testCard = message.value("on", false);
        if (op == "status" || op == "lock" || op == "testcard")
            notify.push_back([this] { listener_.machineChanged(); });
    } else if (type == "switches") {
        receiveSwitches(message);
        notify.push_back([this] { listener_.switchesChanged(); });
    } else if (type == "prepare") {
        std::string text = "The change goes out at the next deck switch";
        if (message.value("op", "") == "sent") {
            const auto skipped = message.value("skipped", std::vector<std::string>{});
            text = "Sent " + std::to_string(message.value("applied", 0)) + " edits";
            for (size_t i = 0; i < skipped.size(); ++i)
                text += (i ? "; " : "; left out ") + skipped[i];
        }
        notify.push_back([this, text] { listener_.prepareNotice(text); });
    } else if (type == "error") {
        const std::string text = message.value("message", "");
        if (message.contains("hash") && message.contains("layer"))
            failed_.insert(message.value("hash", ""));
        // An engine without audio input (a node, the harness) does not serve it, nor a node recording; that is
        // not an error to show.
        if (text != "unknown message type audio" && text != "unknown message type record" &&
            text != "unknown message type machine" && text != "unknown message type settings" &&
            text != "unknown message type switches")
            notify.push_back([this, text] { listener_.engineError(text); });
    }
}

void EngineClient::receiveClock(const Message& message)
{
    clock_.beat = message.value("beat", 0.0);
    clock_.tempo = message.value("tempo", 120.0);
    clock_.budgetLevel = message.value("budgetLevel", uint64_t(0));
    clock_.received = std::chrono::steady_clock::now();
    clock_.clips.clear();
    const Message columns = message.value("columns", Message::object());
    clock_.columns = {columns.value("column", uint64_t(0)), columns.value("next", uint64_t(0)), columns.value("held", 0)};
    clock_.effects.clear();
    auto index = [](const Message& e, const char* key) {
        return e.contains(key) && e[key].is_number() ? std::optional(e[key].get<size_t>()) : std::nullopt;
    };
    for (const auto& e : message.value("effects", Message::array()))
        clock_.effects[e.value("effect", uint64_t(0))] = {index(e, "step"), index(e, "next"), e.value("held", 0)};
    clock_.palette.reset();
    clock_.paletteColours.clear();
    if (auto palette = message.find("palette"); palette != message.end() && palette->is_object()) {
        clock_.palette = ClockState::EffectSteps{index(*palette, "palette"), index(*palette, "next"),
                                                 palette->value("held", 0)};
        for (const auto& c : palette->value("colours", Message::array()))
            if (c.is_array() && c.size() == 4)
                clock_.paletteColours.push_back(c.get<std::array<double, 4>>());
    }
    triggeredSinceClock_.clear();
    for (const auto& clip : message.value("clips", Message::array()))
        if (clip.value("visible", true))
            clock_.clips[clip.value("layer", uint64_t(0))] = {clip.value("column", uint64_t(0)), clip.value("frame", 0),
                                                              clip.value("beats", 0), clip.value("next", uint64_t(0)),
                                                              clip.value("held", 0)};
}

void EngineClient::receiveModulation(const Message& message)
{
    const std::string op = message.value("op", "");
    if (op == "state") {
        ModulationState state;
        for (const auto& m : message.value("macros", Message::array()))
            state.macros[m.value("id", uint64_t(0))] = m.value("value", 0.0);
        for (const auto& m : message.value("modulators", Message::array()))
            state.modulators[m.value("id", uint64_t(0))] = m.value("value", 0.0);
        for (const auto& p : message.value("params", Message::array()))
            state.params[{p.value("layer", uint64_t(0)), p.value("effect", uint64_t(0)), p.value("param", "")}] =
                p.value("value", 0.0);
        state.dangling = message.value("dangling", std::vector<uint64_t>{});
        modulation_ = std::move(state);
    } else if (op == "list") {
        macroSets_.clear();
        const Message sets = message.value("sets", Message::object());
        for (const auto& [name, set] : sets.items())
            macroSets_.push_back(name);
    } else if (op == "save") {
        sendLocked({{"type", "modulation"}, {"op", "list"}});
    } else if (op == "preview") {
        const Message replaces = message.value("replaces", Message::object());
        MacroSetPreview preview{message.value("name", ""), {}, message.value("notes", std::vector<std::string>{}),
                                replaces.value("macros", size_t(0)), replaces.value("modulators", size_t(0))};
        for (const auto& i : message.value("items", Message::array()))
            preview.items.push_back({i.value("key", ""), i.value("kind", ""), i.value("label", ""), i.value("status", ""),
                                     i.value("parent", "")});
        macroSetPreview_ = std::move(preview);
    }
}

void EngineClient::receiveAudio(const Message& message)
{
    AudioLevels levels;
    const Message values = message.value("values", Message::object());
    for (const auto& [name, value] : values.items())
        if (value.is_number())
            levels.values[name] = value.get<double>();
    levels.gainDb = message.value("gainDb", 0.0);
    levels.live = message.value("live", false);
    const Message onsets = message.value("onsets", Message::object());
    for (const auto& [name, value] : onsets.items())
        levels.onsets[name] = value.is_boolean() ? value.get<bool>() : value.is_number() && value.get<double>() > 0.0;
    for (const auto& value : message.value("spectrum", Message::array()))
        if (value.is_number())
            levels.spectrum.push_back(value.get<double>());
    for (const auto& c : message.value("channels", Message::array()))
        levels.channels.push_back({c.value("levelDb", -120.0), c.value("noise", 0.0)});
    audio_ = std::move(levels);
}

namespace {

std::optional<double> optionalNumber(const Message& object, const char* key)
{
    return object.is_object() && object.contains(key) && object[key].is_number() ? std::optional(object[key].get<double>())
                                                                                  : std::nullopt;
}

std::string stringOr(const Message& object, const char* key)
{
    return object.is_object() && object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : "";
}

MachineStatus::Media mediaStatus(const Message& media)
{
    MachineStatus::Media m{.ready = media.value("ready", false),
                           .mode = stringOr(media, "override"),
                           .state = stringOr(media, "state"),
                           .error = stringOr(media, "error"),
                           .alert = stringOr(media, "alert")};
    if (const std::optional<double> missing = optionalNumber(media, "missing"))
        m.missing = int(*missing);
    for (const Message& deck : media.value("decks", Message::array()))
        m.decks.emplace_back(stringOr(deck, "name"), deck.value("missing", 0));
    if (const Message download = media.value("download", Message()); download.is_object()) {
        const double size = download.value("size", 0.0);
        m.progress = size > 0.0 ? download.value("bytes", 0.0) / size : 0.0;
    }
    return m;
}

} // namespace

void EngineClient::receiveMachine(const Message& message)
{
    MachineStatus status;
    status.mode = stringOr(message, "mode");
    status.safeReason = stringOr(message, "reason");
    status.showLock = message.value("showLock", false);
    status.testCard = message.value("testCard", false);
    status.version = stringOr(message, "version");
    const Message render = message.value("render", Message::object());
    status.fps = optionalNumber(render, "fps");
    status.gpuMs = optionalNumber(render, "gpuMs");
    status.guard = render.value("guard", uint64_t(0));
    const Message thermal = message.value("thermal", Message::object());
    status.cpuSpeedLimit = optionalNumber(thermal, "cpuSpeedLimit");
    status.temperatureC = optionalNumber(thermal, "temperatureC");
    status.thermalState = stringOr(thermal, "state");
    if (const Message cue = message.value("nextCue", Message()); cue.is_object())
        status.nextCue = MachineStatus::Cue{cue.value("cue", uint64_t(0)), stringOr(cue, "name"),
                                            cue.value("bar", int64_t(0)), cue.value("seconds", 0.0)};
    for (const Message& o : message.value("outputs", Message::array()))
        status.outputs.push_back({o.value("screen", uint64_t(0)), stringOr(o, "screenName"), stringOr(o, "display"),
                                  stringOr(o, "displayName"), optionalNumber(o, "hz"), optionalNumber(o, "presents"),
                                  optionalNumber(o, "hitches"), stringOr(o, "state")});
    if (const Message audio = message.value("audio", Message()); audio.is_object()) {
        const Message channels = audio.value("channels", Message::array());
        status.audio = MachineStatus::Audio{stringOr(audio, "device"),
                                            stringOr(audio, "state"),
                                            channels.is_array() ? channels.get<std::vector<int>>() : std::vector<int>{},
                                            audio.value("gainDb", 0.0),
                                            audio.value("level", 0.0),
                                            audio.value("live", false)};
    }
    const Message clock = message.value("clock", Message::object());
    status.bpm = clock.value("bpm", 0.0);
    status.bar = clock.value("bar", int64_t(0));
    status.beatInBar = clock.value("beatInBar", 0);
    if (const Message djlink = clock.value("djlink", Message()); djlink.is_object()) {
        status.djlinkEnabled = djlink.value("enabled", false);
        status.djlinkFollowing = djlink.value("following", false);
        for (const Message& p : djlink.value("players", Message::array())) {
            const Message onAir = p.value("onAir", Message());
            const std::optional<double> beat = optionalNumber(p, "beatInBar");
            status.djPlayers.push_back({p.value("number", 0),
                                        onAir.is_boolean() ? std::optional<bool>(onAir.get<bool>()) : std::nullopt,
                                        optionalNumber(p, "bpm"), optionalNumber(p, "pitch"), stringOr(p, "play"),
                                        beat ? std::optional<int>(int(*beat)) : std::nullopt});
        }
    }
    if (const Message media = message.value("media", Message()); media.is_object())
        status.media = mediaStatus(media);
    const Message update = message.value("update", Message::object());
    status.updateRefused = stringOr(update, "refused");
    if (const Message last = update.value("last", Message()); last.is_object())
        status.lastUpdate = MachineStatus::Update{stringOr(last, "result"), stringOr(last, "build"),
                                                  stringOr(last, "previous"), stringOr(last, "reason"),
                                                  last.value("at", 0.0)};
    machine_ = std::move(status);
}

void EngineClient::receiveRecord(const Message& message, std::vector<std::function<void()>>& notify)
{
    if (message.value("op", "") == "stopped") {
        record_.recording = false;
        if (message.value("reason", "") == "disk full")
            record_.space = "full";
        if (const std::string notice = message.value("notice", ""); !notice.empty())
            notify.push_back([this, notice] { listener_.recordNotice(notice); });
    } else if (message.value("op", "") == "folders") {
        FolderList list{message.value("path", ""), message.value("parent", ""), message.value("freeMB", uint64_t(0)),
                        message.value("totalMB", uint64_t(0)), message.value("writable", false)};
        for (const auto& f : message.value("folders", Message::array()))
            list.folders.push_back({f.value("name", ""), f.value("path", "")});
        for (const auto& v : message.value("volumes", Message::array()))
            list.volumes.push_back({v.value("name", ""), v.value("path", ""), v.value("freeMB", uint64_t(0)),
                                    v.value("totalMB", uint64_t(0))});
        folders_ = std::move(list);
    } else {
        record_ = {message.value("recording", false), message.value("codec", ""), message.value("target", ""),
                   message.value("file", ""),          message.value("frames", uint64_t(0)),
                   message.value("dropped", uint64_t(0)), message.value("space", ""),
                   message.value("freeMB", uint64_t(0)),  message.value("stopsAtMB", uint64_t(0)),
                   message.value("audio", ""),            message.value("audioReason", ""),
                   message.value("audioSource", ""),      message.value("path", ""),
                   message.value("folder", ""),
                   message.value("folderProblem", ""), message.value("width", uint32_t(0)),
                   message.value("height", uint32_t(0)), message.value("reserveMB", uint64_t(0)),
                   message.value("reservePercent", 0.0), message.value("maxMB", uint64_t(0)),
                   message.value("maxSeconds", 0.0),   message.value("precise", false),
                   message.value("maxTotalSeconds", 0.0), message.value("gigGraceSeconds", 0.0),
                   message.value("stopsAtClock", ""),  message.value("stopsFor", "")};
        if (const std::string notice = message.value("notice", ""); !notice.empty())
            notify.push_back([this, notice] { listener_.recordNotice(notice); });
    }
    notify.push_back([this] { listener_.recordChanged(); });
}

void EngineClient::receiveLibrary(const Message& message, std::vector<std::function<void()>>& notify)
{
    const std::string op = message.value("op", "");
    if (op == "list") {
        library_.clear();
        for (const auto& entry : message.value("entries", Message::array()))
            library_.push_back(libraryItem(entry));
        notify.push_back([this] { listener_.libraryChanged(); });
    } else if (op == "add") {
        const LibraryItem item = libraryItem(message.at("entry"));
        std::erase_if(library_, [&](const LibraryItem& known) { return known.hash == item.hash; });
        library_.push_back(item);
        if (auto it = adding_.find(item.path); it != adding_.end()) {
            notify.push_back([then = std::move(it->second), item] { then(item); });
            adding_.erase(it);
        }
        notify.push_back([this] { listener_.libraryChanged(); });
    } else if (op == "relink") {
        const std::string hash = message.value("hash", "");
        const LibraryItem item = libraryItem(message.at("entry"));
        failed_.erase(hash);
        if (item.hash != hash)
            replaceMedia(hash, item);
        notify.push_back([this] { listener_.libraryChanged(); });
    } else if (op == "still") {
        const std::string hash = message.value("hash", "");
        stills_[hash] = message.contains("jpeg") ? bytesOf(message["jpeg"]) : std::vector<uint8_t>{};
        notify.push_back([this, hash] { listener_.stillArrived(hash); });
    }
}

// Under the lock: every cell holding `hash` takes the new media, as one gesture.
void EngineClient::replaceMedia(const std::string& hash, const LibraryItem& item)
{
    std::vector<Cell> cells;
    forEachLayer(mirror_.composition().layers, [&](const Layer& layer) {
        for (const auto& [column, clip] : layer.clips)
            if (clip.hash == hash)
                cells.push_back({layer.id, column, clip});
    });
    const uint64_t gesture = ++lastGesture_;
    for (Cell& cell : cells) {
        cell.clip.hash = item.hash;
        cell.clip.name = item.name;
        Message command = msg::command(SetSlot{cell.layer, cell.column, cell.clip});
        command["gesture"] = gesture;
        sendLocked(command);
    }
}

void EngineClient::receivePreview(const Message& message, std::vector<std::function<void()>>& notify)
{
    const std::string op = message.value("op", "");
    const std::string source = message.value("source", "");
    if (op == "clips") {
        clipPreviews_ = message.value("on", false);
        notify.push_back([this] { listener_.clipPreviewsChanged(); });
        return;
    }
    if (op == "surfaces") {
        auto ring = std::make_unique<Surfaces>();
        for (const auto& id : message.value("surfaces", Message::array()))
            ring->surfaces.push_back(IOSurfaceLookup(id.get<IOSurfaceID>()));
        surfaces_[source] = std::move(ring);
        return;
    }
    if (op != "frame")
        return;
    PreviewPicture picture;
    picture.frame = message.value("frame", uint64_t(0));
    if (message.contains("jpeg")) {
        picture.width = message.value("width", 0u);
        picture.height = message.value("height", 0u);
        picture.jpeg = bytesOf(message["jpeg"]);
    } else {
        auto ring = surfaces_.find(source);
        if (ring == surfaces_.end() || !ring->second->read(message.value("surface", size_t(0)), picture))
            return;
    }
    previews_[source] = std::move(picture);
    notify.push_back([this, source] { listener_.previewArrived(source); });
}

bool EngineClient::connected() const
{
    std::lock_guard lock(mutex_);
    return bool(send_);
}

bool EngineClient::synced() const
{
    std::lock_guard lock(mutex_);
    return send_ && mirror_.synced();
}

std::string EngineClient::engineBuild() const
{
    std::lock_guard lock(mutex_);
    return build_;
}

std::string EngineClient::versionSkew() const
{
    std::lock_guard lock(mutex_);
    return skew_;
}

bool EngineClient::local() const
{
    std::lock_guard lock(mutex_);
    return local_;
}

std::shared_ptr<const Composition> EngineClient::composition() const
{
    std::lock_guard lock(mutex_);
    return composition_;
}

uint64_t EngineClient::change() const
{
    std::lock_guard lock(mutex_);
    return mirror_.change();
}

ClockState EngineClient::clock() const
{
    std::lock_guard lock(mutex_);
    return clock_;
}

double EngineClient::beatNow() const
{
    std::lock_guard lock(mutex_);
    if (clock_.received == std::chrono::steady_clock::time_point{})
        return clock_.beat;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - clock_.received).count();
    return clock_.beat + seconds * clock_.tempo / 60.0;
}

std::vector<LibraryItem> EngineClient::library() const
{
    std::lock_guard lock(mutex_);
    return library_;
}

std::vector<uint8_t> EngineClient::still(const std::string& hash)
{
    std::lock_guard lock(mutex_);
    if (auto it = stills_.find(hash); it != stills_.end())
        return it->second;
    if (send_ && stillsAsked_.insert(hash).second)
        sendLocked({{"type", "library"}, {"op", "still"}, {"hash", hash}});
    return {};
}

std::optional<PreviewPicture> EngineClient::preview(const std::string& source) const
{
    std::lock_guard lock(mutex_);
    auto it = previews_.find(source);
    if (it == previews_.end())
        return std::nullopt;
    return it->second;
}

std::set<std::string> EngineClient::failedClips() const
{
    std::lock_guard lock(mutex_);
    return failed_;
}

bool EngineClient::playing(uint64_t layer, uint64_t column) const
{
    std::lock_guard lock(mutex_);
    auto it = clock_.clips.find(layer);
    return it != clock_.clips.end() && it->second.column == column;
}

bool EngineClient::queued(uint64_t layer, uint64_t column) const
{
    std::shared_ptr<const Composition> composition;
    ClockState clock;
    bool sinceClock = false;
    {
        std::lock_guard lock(mutex_);
        composition = composition_;
        clock = clock_;
        sinceClock = triggeredSinceClock_.contains(layer);
    }
    const Layer* found = findLayer(*composition, layer);
    if (!found || !found->playing || found->playing->column != column)
        return false;
    if (found->playing->beat && *found->playing->beat > beatNow() + kBeatEpsilon)
        return true;
    auto shown = clock.clips.find(layer);
    return sinceClock && (shown == clock.clips.end() || shown->second.column != column);
}

History EngineClient::history() const
{
    std::lock_guard lock(mutex_);
    if (!prepared_)
        return history_;
    History local{prepared_->undoSteps(), prepared_->redoSteps(), "", "", prepared_->ops(true), prepared_->ops(false)};
    local.undoOp = local.past.empty() ? "" : local.past.back();
    local.redoOp = local.future.empty() ? "" : local.future.back();
    return local;
}

bool EngineClient::unsaved() const
{
    std::lock_guard lock(mutex_);
    return unsavedAtSnapshot_ || history_.undo != savedHistory_.undo || history_.undoOp != savedHistory_.undoOp;
}

std::string EngineClient::savedPath() const
{
    std::lock_guard lock(mutex_);
    return savedPath_;
}

std::vector<AudioDevice> EngineClient::audioDevices() const
{
    std::lock_guard lock(mutex_);
    return audioDevices_;
}

AudioInputState EngineClient::audioInput() const
{
    std::lock_guard lock(mutex_);
    return audioInput_;
}

void EngineClient::selectAudioInput(const std::string& device, const std::vector<int>& channels)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "audio"}, {"op", "select"}, {"device", device}, {"channels", channels}});
}

AudioGain EngineClient::audioGain() const
{
    std::lock_guard lock(mutex_);
    return audioGain_;
}

void EngineClient::setAudioGain(const AudioGain& gain)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "audio"}, {"op", "settings"}, {"autoGain", gain.autoGain}, {"gainDb", gain.gainDb}});
}

void EngineClient::nudge(double percent)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "clock"}, {"op", "nudge"}, {"percent", percent}});
}

void EngineClient::markSection(const std::string& section)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "cue"}, {"op", "mark"}, {"section", section}});
}

void EngineClient::saveComposition(const std::string& path)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "composition"}, {"op", "save"}, {"path", path}});
}

void EngineClient::loadComposition(const std::string& path)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "composition"}, {"op", "load"}, {"path", path}});
}

void EngineClient::requestShows(const std::string& folder)
{
    std::lock_guard lock(mutex_);
    Message m{{"type", "composition"}, {"op", "shows"}};
    if (!folder.empty())
        m["folder"] = folder;
    sendLocked(m);
}

ShowList EngineClient::shows() const
{
    std::lock_guard lock(mutex_);
    return shows_;
}

ModulationState EngineClient::modulation() const
{
    std::lock_guard lock(mutex_);
    return modulation_;
}

AudioLevels EngineClient::audio() const
{
    std::lock_guard lock(mutex_);
    return audio_;
}

// The shaders the engine registered: every effect and generator that parsed, and the transitions that translated.
void EngineClient::receiveIsf(const Message& message, std::vector<std::function<void()>>& notify)
{
    std::vector<IsfSource> sources;
    for (const Message& s : message.value("shaders", Message::array())) {
        if (!s.contains("text") || s.value("type", "").empty())
            return;
        if (s.value("kind", "") != "transition" || s.value("error", "").empty())
            sources.push_back({s.value("name", ""), s.value("text", ""), true});
    }
    mirrorUserIsf(sources);
    notify.push_back([this] { listener_.documentChanged(); });
}

void EngineClient::rescanIsf()
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "isf"}, {"op", "rescan"}, {"text", true}});
}

Message EngineClient::presets() const
{
    std::lock_guard lock(mutex_);
    return presets_;
}

std::vector<PresetEntry> EngineClient::presetEntries() const
{
    std::lock_guard lock(mutex_);
    return presetEntries_;
}

std::map<std::string, std::string> EngineClient::presetDefaults() const
{
    std::lock_guard lock(mutex_);
    return presetDefaults_;
}

Message EngineClient::chainPresets() const
{
    std::lock_guard lock(mutex_);
    return chainPresets_;
}

Message EngineClient::presetFile() const
{
    std::lock_guard lock(mutex_);
    Message mine = Message::object();
    for (const PresetEntry& entry : presetEntries_)
        if (entry.group == "mine")
            mine[entry.effectType][entry.name] = entry.params;
    return {{"version", 1}, {"presets", mine}, {"defaults", presetDefaults_}, {"chains", chainPresets_}};
}

void EngineClient::receivePresets(const Message& message)
{
    presets_ = message.value("presets", Message::object());
    presetDefaults_.clear();
    const Message defaults = message.value("defaults", Message::object());
    for (const auto& [type, name] : defaults.items())
        if (name.is_string())
            presetDefaults_[type] = name.get<std::string>();
    chainPresets_ = message.value("chains", Message::object());
    presetEntries_.clear();
    auto params = [](const Message& values) {
        std::map<std::string, double> out;
        for (const auto& [name, value] : values.items())
            if (value.is_number())
                out[name] = value.get<double>();
        return out;
    };
    if (!message.contains("entries")) {
        for (const auto& [type, named] : presets_.items())
            for (const auto& [name, values] : named.items())
                presetEntries_.push_back({type, name, "mine", params(values)});
        return;
    }
    for (const auto& entry : message["entries"])
        presetEntries_.push_back({entry.value("effectType", ""), entry.value("name", ""), entry.value("group", ""),
                                  params(entry.value("params", Message::object()))});
}

std::vector<std::string> EngineClient::macroSets() const
{
    std::lock_guard lock(mutex_);
    return macroSets_;
}

std::optional<MacroSetPreview> EngineClient::macroSetPreview() const
{
    std::lock_guard lock(mutex_);
    return macroSetPreview_;
}

std::vector<OutputDisplay> EngineClient::displays() const
{
    std::lock_guard lock(mutex_);
    return displays_;
}

bool EngineClient::unmirror() const
{
    std::lock_guard lock(mutex_);
    return unmirror_;
}

void EngineClient::setUnmirror(bool on)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "outputs"}, {"op", "unmirror"}, {"on", on}});
}

bool EngineClient::clipPreviews() const
{
    std::lock_guard lock(mutex_);
    return clipPreviews_;
}

void EngineClient::setClipPreviews(bool on)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "preview"}, {"op", "clips"}, {"on", on}});
}

void EngineClient::showGuides(bool shown)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "outputs"}, {"op", "guides"}, {"show", shown}});
}

void EngineClient::identify(const std::string& display, double seconds)
{
    Message message = {{"type", "outputs"}, {"op", "identify"}, {"seconds", seconds}};
    if (!display.empty())
        message["display"] = display;
    std::lock_guard lock(mutex_);
    sendLocked(message);
}

void EngineClient::relink(const std::string& hash, const std::string& path)
{
    Message message = {{"type", "library"}, {"op", "relink"}, {"hash", hash}};
    if (!path.empty())
        message["path"] = path;
    std::lock_guard lock(mutex_);
    sendLocked(message);
}

void EngineClient::stopLayer(uint64_t layer)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "trigger"}, {"op", "stop"}, {"layer", layer}});
}

void EngineClient::setTarget(const std::string& display, std::optional<Point> point)
{
    Message message = {{"type", "outputs"}, {"op", "target"}, {"display", display}, {"point", nullptr}};
    if (point)
        message["point"] = *point;
    std::lock_guard lock(mutex_);
    sendLocked(message);
}

std::optional<MachineStatus> EngineClient::machine() const
{
    std::lock_guard lock(mutex_);
    return machine_;
}

EngineClient::Switches EngineClient::switches() const
{
    std::lock_guard lock(mutex_);
    return switches_;
}

// A line of another night starts that night's list.
void EngineClient::receiveSwitches(const Message& message)
{
    const std::string date = message.value("date", "");
    if (message.value("op", "") == "list") {
        switches_ = {date, message.value("switches", std::vector<Message>{})};
        return;
    }
    if (date != switches_.date)
        switches_ = {date, {}};
    switches_.lines.push_back(message.value("switch", Message::object()));
}

void EngineClient::watchMachine(bool on)
{
    std::lock_guard lock(mutex_);
    if (on == watchingMachine_)
        return;
    watchingMachine_ = on;
    if (on)
        sendLocked({{"type", "machine"}, {"op", "subscribe"}, {"rate", kMachineRate}});
    else
        sendLocked({{"type", "machine"}, {"op", "unsubscribe"}});
}

bool EngineClient::watchingMachine() const
{
    std::lock_guard lock(mutex_);
    return watchingMachine_;
}

void EngineClient::watchVideo(bool on)
{
    {
        std::lock_guard lock(mutex_);
        if (on == watchingVideo_)
            return;
        watchingVideo_ = on;
        sendLocked({{"type", "video"}, {"op", "watch"}, {"on", on}});
    }
    if (!on)
        video_.clear();
}

bool EngineClient::watchingVideo() const
{
    std::lock_guard lock(mutex_);
    return watchingVideo_;
}

void EngineClient::listen(bool on)
{
    std::lock_guard lock(mutex_);
    if (on == listening_)
        return;
    listening_ = on;
    sendLocked({{"type", "listen"}, {"op", on ? "start" : "stop"}});
}

bool EngineClient::listening() const
{
    std::lock_guard lock(mutex_);
    return listening_;
}

void EngineClient::setListenSink(std::function<void(const Message&)> sink)
{
    std::lock_guard lock(mutex_);
    listenSink_ = std::move(sink);
}

void EngineClient::machineAction(Message action)
{
    action["type"] = "machine";
    std::lock_guard lock(mutex_);
    sendLocked(action);
}

void EngineClient::setMediaSync(const std::string& mode)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "mediaSync"}, {"op", "set"}, {"override", mode}});
}

void EngineClient::sendUpdate(std::filesystem::path app)
{
    std::lock_guard lock(mutex_);
    if (update_.state == "packing" || update_.state == "sending")
        return;
    if (updater_.joinable())
        updater_.join();
    update_ = {.state = "packing"};
    updater_ = std::thread([this, app = std::move(app)] { runUpdate(app); });
}

UpdateProgress EngineClient::updateProgress() const
{
    std::lock_guard lock(mutex_);
    return update_;
}

void EngineClient::setUpdate(UpdateProgress progress)
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
            return;
        update_ = std::move(progress);
    }
    listener_.machineChanged();
}

std::optional<Message> EngineClient::exchange(const Message& request)
{
    std::unique_lock lock(mutex_);
    if (!send_)
        return std::nullopt;
    updateReply_.reset();
    sendLocked(request);
    updateReplied_.wait_for(lock, std::chrono::seconds(30), [&] { return updateReply_ || stopping_ || !send_; });
    return std::exchange(updateReply_, std::nullopt);
}

void EngineClient::runUpdate(const std::filesystem::path& app)
{
    listener_.machineChanged();
    UpdateProgress progress{.state = "failed"};
    try {
        BundlePush push(app);
        progress = {.state = "sending", .size = push.size(), .build = push.build()};
        setUpdate(progress);
        std::optional<Message> request = push.start();
        while (request) {
            const std::optional<Message> reply = exchange(*request);
            if (!reply)
                throw std::runtime_error("the engine stopped answering");
            if (reply->value("type", "") == "error")
                throw std::runtime_error(reply->value("message", ""));
            if (reply->value("state", "") == "installing") {
                progress.state = "installing";
                progress.sent = progress.size;
                return setUpdate(progress);
            }
            progress.sent = reply->value("received", uint64_t(0));
            setUpdate(progress);
            request = push.next();
        }
        throw std::runtime_error("the engine did not take the whole bundle");
    } catch (const std::exception& e) {
        progress.state = "failed";
        progress.error = e.what();
        setUpdate(progress);
    }
}

void EngineClient::requestOutputs()
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "outputs"}, {"op", "list"}});
}

void EngineClient::showLocked()
{
    composition_ = std::make_shared<const Composition>(prepared_ ? prepared_->composition() : mirror_.composition());
}

// False when not in Prepare.
bool EngineClient::editLocally(const std::function<void(PreparedCopy&)>& change)
{
    std::string error;
    {
        std::lock_guard lock(mutex_);
        if (!prepared_)
            return false;
        try {
            change(*prepared_);
            showLocked();
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    if (!error.empty())
        listener_.engineError(error);
    listener_.documentChanged();
    return true;
}

void EngineClient::prepare()
{
    {
        std::lock_guard lock(mutex_);
        if (prepared_ || !mirror_.synced())
            return;
        prepared_ = std::make_unique<PreparedCopy>(mirror_.composition());
        showLocked();
    }
    listener_.documentChanged();
}

bool EngineClient::preparing() const
{
    std::lock_guard lock(mutex_);
    return bool(prepared_);
}

void EngineClient::sendPrepared(bool atSwitch)
{
    {
        std::lock_guard lock(mutex_);
        if (!prepared_)
            return;
        if (!prepared_->empty())
            sendLocked({{"type", "prepare"},
                        {"op", "send"},
                        {"at", atSwitch ? "switch" : "now"},
                        {"edits", preparedToJson(prepared_->edits())}});
        prepared_.reset();
        showLocked();
    }
    listener_.documentChanged();
}

void EngineClient::discardPrepared()
{
    {
        std::lock_guard lock(mutex_);
        if (!prepared_)
            return;
        prepared_.reset();
        showLocked();
    }
    listener_.documentChanged();
}

void EngineClient::edit(const Edit& edit)
{
    if (editLocally([&](PreparedCopy& copy) { copy.edit(edit, gesture_); }))
        return;
    std::lock_guard lock(mutex_);
    Message command = msg::command(edit);
    if (gesture_)
        command["gesture"] = gesture_;
    sendLocked(command);
}

void EngineClient::beginGesture()
{
    std::lock_guard lock(mutex_);
    gesture_ = ++lastGesture_;
}

void EngineClient::endGesture()
{
    std::lock_guard lock(mutex_);
    gesture_ = 0;
}

bool EngineClient::gesturing() const
{
    std::lock_guard lock(mutex_);
    return gesture_ != 0;
}

void EngineClient::requestModulation()
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "modulation"}, {"op", "state"}});
}

void EngineClient::savePreset(uint64_t effect, const std::string& name)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "preset"}, {"op", "save"}, {"effect", effect}, {"name", name}});
}

void EngineClient::applyPreset(uint64_t effect, const std::string& name, const std::string& group)
{
    std::lock_guard lock(mutex_);
    Message message{{"type", "preset"}, {"op", "apply"}, {"effect", effect}, {"name", name}};
    if (!group.empty())
        message["group"] = group;
    sendLocked(message);
}

void EngineClient::renamePreset(const std::string& effectType, const std::string& name, const std::string& to)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "preset"}, {"op", "rename"}, {"effectType", effectType}, {"name", name}, {"to", to}});
}

void EngineClient::deletePreset(const std::string& effectType, const std::string& name)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "preset"}, {"op", "delete"}, {"effectType", effectType}, {"name", name}});
}

void EngineClient::importPresets()
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "preset"}, {"op", "import"}});
}

void EngineClient::setDefaultPreset(const std::string& effectType, const std::string& name)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "preset"}, {"op", "default"}, {"effectType", effectType}, {"name", name}});
}

void EngineClient::saveChainPreset(const std::string& name, const std::vector<Effect>& effects)
{
    Message list = Message::array();
    for (Effect effect : effects) {
        effect.id = 0;
        effect.links.clear();
        list.push_back(effect);
    }
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "preset"}, {"op", "saveChain"}, {"name", name}, {"effects", list}});
}

void EngineClient::renameChainPreset(const std::string& name, const std::string& to)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "preset"}, {"op", "renameChain"}, {"name", name}, {"to", to}});
}

void EngineClient::deleteChainPreset(const std::string& name)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "preset"}, {"op", "deleteChain"}, {"name", name}});
}

void EngineClient::mergePresets(const Message& file)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "preset"}, {"op", "merge"}, {"file", file}});
}

void EngineClient::saveMacroSet(const std::string& name)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "modulation"}, {"op", "save"}, {"name", name}});
}

void EngineClient::previewMacroSet(const std::string& name)
{
    std::lock_guard lock(mutex_);
    macroSetPreview_.reset();
    sendLocked({{"type", "modulation"}, {"op", "preview"}, {"name", name}});
}

void EngineClient::loadMacroSet(const std::string& name, const std::vector<std::string>& drop)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "modulation"}, {"op", "load"}, {"name", name}, {"drop", drop}});
}

void EngineClient::undo()
{
    if (editLocally([](PreparedCopy& copy) { copy.undo(); }))
        return;
    std::lock_guard lock(mutex_);
    sendLocked(msg::undo());
}

void EngineClient::redo()
{
    if (editLocally([](PreparedCopy& copy) { copy.redo(); }))
        return;
    std::lock_guard lock(mutex_);
    sendLocked(msg::redo());
}

void EngineClient::triggerClip(uint64_t layer, uint64_t column)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "trigger"}, {"op", "clip"}, {"layer", layer}, {"column", column}});
}

void EngineClient::triggerColumn(uint64_t column)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "trigger"}, {"op", "column"}, {"column", column}});
}

void EngineClient::triggerStep(uint64_t effect, size_t step)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "trigger"}, {"op", "step"}, {"effect", effect}, {"step", step}});
}

void EngineClient::selectDeck(uint64_t deck)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "trigger"}, {"op", "deck"}, {"deck", deck}, {"by", "ui"}});
}

void EngineClient::selectPalette(uint64_t palette)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "trigger"}, {"op", "palette"}, {"palette", palette}});
}

void EngineClient::cancelTrigger(std::optional<uint64_t> layer)
{
    Message message = {{"type", "trigger"}, {"op", "cancel"}};
    if (layer)
        message["layer"] = *layer;
    std::lock_guard lock(mutex_);
    sendLocked(message);
}

RecordState EngineClient::record() const
{
    std::lock_guard lock(mutex_);
    return record_;
}

void EngineClient::record(bool on)
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "record"}, {"op", on ? "start" : "stop"}});
}

void EngineClient::setRecordSettings(const Message& fields)
{
    Message message = fields;
    message["type"] = "record";
    message["op"] = "set";
    std::lock_guard lock(mutex_);
    sendLocked(message);
}

void EngineClient::requestFolders(const std::string& path)
{
    Message message{{"type", "record"}, {"op", "folders"}};
    if (!path.empty())
        message["path"] = path;
    std::lock_guard lock(mutex_);
    sendLocked(message);
}

void EngineClient::receiveSettings(const Message& message)
{
    EngineSettings s;
    s.profile = message.value("profile", "smooth");
    s.protectTransitions = message.value("protectTransitions", false);
    s.bakeStatic = message.value("bakeStatic", false);
    s.renderScale = message.value("renderScale", uint64_t(100));
    s.frameCacheMB = message.value("frameCacheMB", uint64_t(0));
    s.preloadMB = message.value("preloadMB", uint64_t(0));
    s.clipPreviews = message.value("clipPreviews", uint64_t(0));
    const Message use = message.value("use", Message::object());
    s.use = {use.value("frameCacheMB", uint64_t(0)), use.value("preloadMB", uint64_t(0)),
             use.value("clipPreviews", uint64_t(0)), use.value("clipPreviewKB", uint64_t(0))};
    for (const auto& [field, flag] : message.value("overridden", Message::object()).items())
        if (flag.is_string())
            s.overridden[field] = flag.get<std::string>();
    for (const Message& section : message.value("sections", Message::array()))
        if (section.is_string())
            s.sections.push_back(section.get<std::string>());
    settings_ = std::move(s);
}

EngineSettings EngineClient::settings() const
{
    std::lock_guard lock(mutex_);
    return settings_;
}

void EngineClient::requestSettings()
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "settings"}, {"op", "state"}});
}

void EngineClient::setSettings(const Message& fields)
{
    Message message = fields;
    message["type"] = "settings";
    message["op"] = "set";
    std::lock_guard lock(mutex_);
    sendLocked(message);
}

FolderList EngineClient::folders() const
{
    std::lock_guard lock(mutex_);
    return folders_;
}

void EngineClient::tap()
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "clock"}, {"op", "tap"}});
}

void EngineClient::resync()
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "clock"}, {"op", "resync"}});
}

void EngineClient::requestClock()
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "clock"}, {"op", "state"}});
}

void EngineClient::requestLibrary()
{
    std::lock_guard lock(mutex_);
    sendLocked({{"type", "library"}, {"op", "list"}});
}

void EngineClient::addMedia(const std::vector<std::string>& paths, std::function<void(const LibraryItem&)> then)
{
    std::lock_guard lock(mutex_);
    for (const std::string& path : paths) {
        const std::string normal = std::filesystem::absolute(path).lexically_normal().string();
        adding_[normal] = then;
        sendLocked({{"type", "library"}, {"op", "add"}, {"path", normal}});
    }
}

void EngineClient::watchPreviews(const std::set<std::string>& sources)
{
    std::lock_guard lock(mutex_);
    if (sources == watched_)
        return;
    watched_ = sources;
    dropUnwatchedLocked();
    sendWatchLocked();
}

void EngineClient::watchClipPreviews(const std::set<std::string>& hashes,
                                     std::optional<std::pair<uint64_t, uint64_t>> cell)
{
    std::set<std::string> sources;
    for (const std::string& hash : hashes)
        sources.insert("clip:" + hash);
    std::lock_guard lock(mutex_);
    if (sources == watchedClips_ && cell == watchedCell_)
        return;
    watchedClips_ = std::move(sources);
    watchedCell_ = cell;
    dropUnwatchedLocked();
    sendClipWatchLocked();
}

void EngineClient::dropUnwatchedLocked()
{
    auto unwatched = [&](const std::string& source) {
        return !watched_.contains(source) && !watchedClips_.contains(source);
    };
    std::erase_if(previews_, [&](const auto& preview) { return unwatched(preview.first); });
    std::erase_if(surfaces_, [&](const auto& ring) { return unwatched(ring.first); });
}

void EngineClient::sendClipWatchLocked()
{
    if (!send_)
        return;
    std::erase_if(surfaces_, [](const auto& ring) { return ring.first.starts_with("clip:"); });
    Message watch{{"type", "preview"},
                  {"op", "watch"},
                  {"watch", "clips"},
                  {"sources", std::vector<std::string>(watchedClips_.begin(), watchedClips_.end())},
                  {"via", local_ ? "surface" : "jpeg"},
                  {"width", kClipWidth},
                  {"height", kClipHeight},
                  {"fps", kClipFps}};
    if (watchedCell_)
        watch["clip"] = {{"layer", watchedCell_->first}, {"column", watchedCell_->second}};
    sendLocked(watch);
}

void EngineClient::sendWatchLocked()
{
    if (!send_)
        return;
    std::erase_if(surfaces_, [](const auto& ring) { return !ring.first.starts_with("clip:"); });
    sendLocked({{"type", "preview"},
                {"op", "watch"},
                {"sources", std::vector<std::string>(watched_.begin(), watched_.end())},
                {"via", local_ ? "surface" : "jpeg"},
                {"width", local_ ? kSurfaceWidth : kJpegWidth},
                {"height", local_ ? kSurfaceHeight : kJpegHeight},
                {"fps", local_ ? 30 : 12}});
}

void EngineClient::sendLocked(const Message& message)
{
    if (send_)
        send_(message);
}

} // namespace azl::ui
