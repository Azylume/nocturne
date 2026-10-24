#include "protocol/document_server.hpp"

#include "effects/effect_registry.hpp"
#include "protocol/build_info.hpp"
#include "protocol/prepare.hpp"
#include "protocol/remote_files.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <limits>

namespace azl {

namespace {

std::string stringField(const Message& message, const char* key)
{
    auto it = message.find(key);
    if (it == message.end() || !it->is_string())
        throw ProtocolError(std::string("missing field ") + key);
    return it->get<std::string>();
}

Message error(const std::string& what)
{
    return {{"type", "error"}, {"message", what}};
}

template <typename T>
bool sameValue(const T&, const T&)
{
    return false;
}
bool sameValue(const SetLayerOpacity& a, const SetLayerOpacity& b) { return a.layer == b.layer; }
bool sameValue(const SetEffectParam& a, const SetEffectParam& b)
{
    return a.effect == b.effect && a.param == b.param;
}
bool sameValue(const SetEffectParams& a, const SetEffectParams& b) { return a.effect == b.effect; }
bool sameValue(const SetMacroValue& a, const SetMacroValue& b) { return a.macro == b.macro; }
bool sameValue(const SetMacroTarget& a, const SetMacroTarget& b) { return a.macro == b.macro && a.index == b.index; }
bool sameValue(const SetModulator& a, const SetModulator& b) { return a.modulator.id == b.modulator.id; }
bool sameValue(const SetSliceCorners& a, const SetSliceCorners& b) { return a.slice == b.slice; }
bool sameValue(const SetTempo&, const SetTempo&) { return true; }
bool sameValue(const SetPalette& a, const SetPalette& b) { return a.palette.id && a.palette.id == b.palette.id; }
bool sameValue(const SetSensitivity& a, const SetSensitivity& b) { return a.source == b.source; }
bool sameValue(const SetBandEdges&, const SetBandEdges&) { return true; }
bool sameValue(const SetClipTransport& a, const SetClipTransport& b)
{
    return a.layer == b.layer && a.column == b.column && !a.sync && !b.sync && !a.playMode && !b.playMode && !a.beats &&
           !b.beats && !a.jumpInterval && !b.jumpInterval && !a.jumpDistance && !b.jumpDistance &&
           a.speed.has_value() == b.speed.has_value() && a.in.has_value() == b.in.has_value() &&
           a.out.has_value() == b.out.has_value();
}

bool sameValue(const Batch& a, const Batch& b)
{
    return std::equal(a.edits.begin(), a.edits.end(), b.edits.begin(), b.edits.end(), sameEdit);
}

constexpr uint64_t kNeverSaved = std::numeric_limits<uint64_t>::max();

std::string opOf(const Edit& edit)
{
    return editToJson(edit).value("op", "");
}

} // namespace

bool sameEdit(const Edit& a, const Edit& b)
{
    return a.index() == b.index() &&
           std::visit([&](const auto& x) { return sameValue(x, std::get<std::decay_t<decltype(x)>>(b)); }, a);
}

DocumentServer::DocumentServer(Composition initial, Publish publish)
    : document_(std::move(initial)), publish_(std::move(publish)), deck_(document_.deck)
{
    publish_(std::make_shared<const Composition>(document_));
}

ClientId DocumentServer::connect(Sink sink, bool loopback)
{
    std::lock_guard lock(mutex_);
    const ClientId id = nextClient_++;
    clients_[id] = {std::move(sink)};
    clients_[id].loopback = loopback;
    return id;
}

void DocumentServer::disconnect(ClientId client)
{
    std::function<void(ClientId)> watch;
    {
        std::lock_guard lock(mutex_);
        clients_.erase(client);
        watch = watchDisconnects_;
    }
    if (watch)
        watch(client);
}

void DocumentServer::receive(ClientId id, const Message& message)
{
    bool loopback = false;
    Handler handler = route(id, message, loopback);
    if (!handler)
        return;
    if (!loopback) {
        std::function<std::vector<std::filesystem::path>()> roots;
        {
            std::lock_guard lock(mutex_);
            roots = remoteRoots_;
        }
        const std::vector<std::filesystem::path> allowed = roots ? roots() : std::vector<std::filesystem::path>{};
        if (std::optional<std::string> refused = remotePathRefusal(message, allowed)) {
            send(id, error(*refused));
            return;
        }
    }
    Message withSender = message;
    withSender["client"] = id;
    withSender["loopback"] = loopback;
    handler(withSender, [this, id](const Message& reply) { send(id, reply); });
}

uint64_t DocumentServer::change()
{
    std::lock_guard lock(mutex_);
    return change_;
}

Composition DocumentServer::document()
{
    std::lock_guard lock(mutex_);
    return document_;
}

void DocumentServer::setReadOnly(std::optional<std::string> reason, std::set<std::string> open)
{
    std::lock_guard lock(mutex_);
    readOnly_ = std::move(reason);
    open_ = std::move(open);
}

void DocumentServer::handle(const std::string& type, Handler handler)
{
    std::lock_guard lock(mutex_);
    if (handler)
        handlers_[type] = std::move(handler);
    else
        handlers_.erase(type);
}

bool DocumentServer::send(ClientId id, const Message& message)
{
    std::lock_guard lock(mutex_);
    auto it = clients_.find(id);
    if (it == clients_.end())
        return false;
    it->second.sink(message);
    return true;
}

void DocumentServer::broadcast(const Message& message)
{
    std::lock_guard lock(mutex_);
    for (auto& [id, client] : clients_)
        if (client.watching)
            client.sink(message);
}

void DocumentServer::perform(Edit edit, const char* cause, ClientId client)
{
    std::lock_guard lock(mutex_);
    apply(client, edit, cause);
    followDeck();
}

// Handles a document message in place; returns the handler of a machine-local one, to run outside the lock.
DocumentServer::Handler DocumentServer::route(ClientId id, const Message& message, bool& loopback)
{
    std::lock_guard lock(mutex_);
    auto it = clients_.find(id);
    if (it == clients_.end())
        return {};
    Client& client = it->second;
    loopback = client.loopback;
    try {
        const std::string type = stringField(message, "type");
        if (type == "hello") {
            client.helloed = true;
            client.watching = !(message.contains("watch") && message["watch"] == false);
            if (client.watching)
                sendSnapshot(client, id);
        } else if (!client.helloed) {
            throw ProtocolError("send hello first");
        } else if (type == "snapshot") {
            sendSnapshot(client, id);
        } else if (readOnly_ && !open_.contains(type)) {
            throw ProtocolError(*readOnly_);
        } else if (type == "command") {
            command(id, message);
            followDeck();
        } else if (type == "prepare") {
            prepare(id, message);
            followDeck();
        } else if (auto handler = handlers_.find(type); handler != handlers_.end()) {
            return handler->second;
        } else {
            throw ProtocolError("unknown message type " + type);
        }
    } catch (const ProtocolError& e) {
        client.sink(error(e.what()));
    }
    return {};
}

void DocumentServer::sendSnapshot(Client& client, ClientId id)
{
    Message snapshot{{"type", "snapshot"},
                     {"change", change_},
                     {"client", id},
                     {"composition", document_},
                     {"history", historyJson()},
                     {"steps", stepsJson()},
                     {"build", buildId()},
                     {"effects", effectRegistryVersion()}};
    if (const std::optional<ShowFile> file = fileLocked())
        snapshot["file"] = {{"path", file->path}, {"unsaved", file->unsaved}};
    client.sink(snapshot);
}

std::optional<ShowFile> DocumentServer::file()
{
    std::lock_guard lock(mutex_);
    return fileLocked();
}

void DocumentServer::markSaved(const std::string& path)
{
    std::lock_guard lock(mutex_);
    markSavedLocked(path);
}

void DocumentServer::loadFile(ClientId client, Composition composition, const std::string& path)
{
    std::lock_guard lock(mutex_);
    Edit edit = LoadComposition{std::move(composition)};
    Edit inverse = applyEdit(document_, edit);
    record(client, 0, edit, std::move(inverse));
    announce(client, edit, "edit");
    markSavedLocked(path);
    followDeck();
}

void DocumentServer::markSavedLocked(const std::string& path)
{
    file_ = path;
    savedStep_ = lastStep();
    fileChanged();
}

void DocumentServer::resumeFile(const ShowFile& file)
{
    std::lock_guard lock(mutex_);
    file_ = file.path;
    savedStep_ = file.unsaved ? kNeverSaved : lastStep();
    fileChanged();
}

void DocumentServer::watchFile(FileWatch watch)
{
    std::lock_guard lock(mutex_);
    watchFile_ = std::move(watch);
}

void DocumentServer::watchDisconnects(std::function<void(ClientId)> watch)
{
    std::lock_guard lock(mutex_);
    watchDisconnects_ = std::move(watch);
}

void DocumentServer::allowRemoteFiles(std::function<std::vector<std::filesystem::path>()> roots)
{
    std::lock_guard lock(mutex_);
    remoteRoots_ = std::move(roots);
}

uint64_t DocumentServer::lastStep() const
{
    return undo_.empty() ? 0 : undo_.back().serial;
}

std::optional<ShowFile> DocumentServer::fileLocked() const
{
    if (!file_)
        return std::nullopt;
    return ShowFile{*file_, lastStep() != savedStep_};
}

void DocumentServer::fileChanged()
{
    if (watchFile_)
        watchFile_(fileLocked());
}

// Every step of both stacks by the op of its first edit, oldest first, so a client joining late shows the whole
// history; changes then keep it current through "history".
Message DocumentServer::stepsJson() const
{
    auto ops = [](const std::vector<Step>& steps) {
        Message list = Message::array();
        for (const Step& step : steps)
            list.push_back(opOf(step.redo.front()));
        return list;
    };
    return {{"undo", ops(undo_)}, {"redo", ops(redo_)}};
}

Message DocumentServer::historyJson() const
{
    return {{"undo", undo_.size()},
            {"redo", redo_.size()},
            {"undoOp", undo_.empty() ? "" : opOf(undo_.back().redo.front())},
            {"redoOp", redo_.empty() ? "" : opOf(redo_.back().redo.front())}};
}

void DocumentServer::command(ClientId client, const Message& message)
{
    const std::string op = stringField(message, "op");
    if (op == "undo")
        return history(client, undo_, redo_, true);
    if (op == "redo")
        return history(client, redo_, undo_, false);
    Edit edit = editFromJson(message);
    if (isLive(edit))
        throw ProtocolError(op + " is a live action, sent as a trigger message");
    auto gesture = message.find("gesture");
    if (gesture != message.end() && !gesture->is_number_unsigned())
        throw ProtocolError("bad field gesture");
    if (std::holds_alternative<SetScreenDisplay>(edit)) {
        apply(client, edit, "edit");
        return;
    }
    std::vector<std::string> dropped;
    Edit inverse = applyEdit(document_, edit, &dropped);
    record(client, gesture == message.end() ? 0 : gesture->get<uint64_t>(), edit, std::move(inverse));
    announce(client, edit, "edit", dropped);
}

void DocumentServer::prepare(ClientId client, const Message& message)
{
    if (stringField(message, "op") != "send")
        throw ProtocolError("unknown prepare op");
    const std::string at = stringField(message, "at");
    auto edits = message.find("edits");
    if (edits == message.end())
        throw ProtocolError("missing field edits");
    if (at == "now") {
        clients_.at(client).sink(applyPrepared(client, *edits, std::nullopt, "edit"));
        return;
    }
    if (at != "switch")
        throw ProtocolError("a prepared change goes now or at the next deck switch");
    Cue cue;
    cue.name = "Prepared change";
    cue.prepared = preparedToJson(preparedFromJson(*edits));
    Edit edit = AddCue{std::nullopt, std::move(cue)};
    Edit inverse = applyEdit(document_, edit);
    record(client, 0, edit, std::move(inverse));
    announce(client, edit, "edit");
    clients_.at(client).sink({{"type", "prepare"}, {"op", "scheduled"}, {"cue", std::get<AddCue>(edit).cue.id}});
}

// The edits that still fit, with the Cue that held them removed first, as one undo step.
Message DocumentServer::applyPrepared(ClientId client, const Message& edits, std::optional<uint64_t> cue,
                                      const char* cause)
{
    Composition venue = document_;
    Batch batch;
    if (cue) {
        Edit remove = RemoveCue{*cue};
        applyEdit(venue, remove);
        batch.edits.push_back(RemoveCue{*cue});
    }
    Rebased rebased = rebase(venue, preparedFromJson(edits), cue.has_value());
    batch.edits.insert(batch.edits.end(), rebased.applied.begin(), rebased.applied.end());
    if (!batch.edits.empty()) {
        Edit edit = std::move(batch);
        std::vector<std::string> dropped;
        Edit inverse = applyEdit(document_, edit, &dropped);
        record(client, 0, edit, std::move(inverse));
        announce(client, edit, cause, dropped);
    }
    Message sent{{"type", "prepare"},
                 {"op", "sent"},
                 {"applied", rebased.applied.size()},
                 {"skipped", rebased.skipped},
                 {"change", change_}};
    if (cue)
        sent["cue"] = *cue;
    return sent;
}

// A deck switch applies the prepared changes waiting for it.
void DocumentServer::followDeck()
{
    if (document_.deck == deck_)
        return;
    deck_ = document_.deck;
    std::vector<Cue> waiting;
    std::copy_if(document_.cues.begin(), document_.cues.end(), std::back_inserter(waiting),
                 [](const Cue& cue) { return cue.prepared.has_value(); });
    for (const Cue& cue : waiting) {
        try {
            const Message sent = applyPrepared(0, *cue.prepared, cue.id, "cue");
            for (auto& [id, c] : clients_)
                if (c.helloed)
                    c.sink(sent);
            for (const auto& skipped : sent["skipped"])
                std::printf("prepare: left out %s\n", skipped.get<std::string>().c_str());
        } catch (const std::exception& e) {
            std::printf("prepare: the change of cue %llu did not apply: %s\n", (unsigned long long)cue.id, e.what());
        }
    }
}

void DocumentServer::record(ClientId client, uint64_t gesture, Edit edit, Edit inverse)
{
    if (gesture && redo_.empty() && !undo_.empty() && undo_.back().client == client &&
        undo_.back().gesture == gesture) {
        Step& step = undo_.back();
        step.serial = ++serial_;
        if (sameEdit(step.redo.back(), edit)) {
            step.redo.back() = std::move(edit);
            return;
        }
        step.redo.push_back(std::move(edit));
        step.undo.push_back(std::move(inverse));
        return;
    }
    undo_.push_back({{std::move(edit)}, {std::move(inverse)}, client, gesture, ++serial_});
    redo_.clear();
}

// A step's edits apply on a copy first, so a step that no longer fits leaves the document and both stacks untouched.
void DocumentServer::history(ClientId client, std::vector<Step>& from, std::vector<Step>& to, bool undo)
{
    if (from.empty())
        throw ProtocolError(undo ? "nothing to undo" : "nothing to redo");
    Step step = from.back();
    std::vector<Edit> edits = undo ? std::vector<Edit>(step.undo.rbegin(), step.undo.rend()) : step.redo;
    Composition trial = document_;
    for (Edit edit : edits)
        applyEdit(trial, edit);
    from.pop_back();
    step.gesture = 0;
    to.push_back(std::move(step));
    for (Edit& edit : edits)
        apply(client, edit, undo ? "undo" : "redo");
}

Edit DocumentServer::apply(ClientId client, Edit& edit, const char* cause)
{
    std::vector<std::string> dropped;
    Edit inverse = applyEdit(document_, edit, &dropped);
    announce(client, edit, cause, dropped);
    return inverse;
}

void DocumentServer::announce(ClientId client, const Edit& edit, const char* cause,
                              const std::vector<std::string>& dropped)
{
    ++change_;
    publish_(std::make_shared<const Composition>(document_));
    fileChanged();

    Message change = editToJson(edit);
    change["type"] = "change";
    change["change"] = change_;
    change["cause"] = cause;
    change["client"] = client;
    change["history"] = historyJson();
    if (!dropped.empty())
        change["notice"] = droppedNotice(dropped);
    for (auto& [id, c] : clients_)
        if (c.watching)
            c.sink(change);
}

} // namespace azl
