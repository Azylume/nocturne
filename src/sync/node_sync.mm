#include "sync/node_sync.hpp"

#include "net/websocket.hpp"
#include "protocol/document_mirror.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>

namespace azl {

namespace {

constexpr auto kBeatPoll = std::chrono::milliseconds(20);
constexpr auto kBeatHeartbeat = std::chrono::milliseconds(500);
constexpr auto kRetry = std::chrono::seconds(1);
constexpr auto kTick = std::chrono::milliseconds(100);

const std::set<std::string> kNodeOpen = {"node", "library", "preview", "audio"};

Message beatMessage(const BeatTimeline& t)
{
    return {{"type", "beat"},    {"time", t.time},   {"beat", t.beat},
            {"bpm", t.bpm},      {"nudge", t.nudge}, {"resyncs", t.resyncs}};
}

BeatTimeline beatFrom(const Message& m)
{
    return {m.at("time").get<double>(), m.at("beat").get<double>(), m.at("bpm").get<double>(),
            m.at("nudge").get<double>(), m.at("resyncs").get<uint64_t>()};
}

Message error(const std::string& what)
{
    return {{"type", "error"}, {"message", what}};
}

std::string readOnlyReason(const std::string& main)
{
    return "this engine is a node following " + (main.empty() ? std::string("the main engine") : main) +
           "; edit on the main engine";
}

} // namespace

std::map<std::string, std::string> mediaOf(const Composition& composition)
{
    std::map<std::string, std::string> media;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        for (const auto& [column, clip] : layer.clips)
            if (!clip.hash.empty())
                media.emplace(clip.hash, clip.name);
    });
    return media;
}

bool hasShow(const Composition& composition)
{
    bool show = false;
    forEachLayer(composition.layers, [&](const Layer& layer) { show = show || !layer.clips.empty(); });
    return show;
}

void to_json(nlohmann::json& j, const NodeStatus& s)
{
    j = {{"connected", s.connected}, {"main", s.main},         {"synced", s.synced}, {"held", s.held},
         {"missing", s.missing},     {"fetching", s.fetching}, {"bytes", s.bytes},
         {"files", s.files},         {"roundTrip", s.roundTrip}};
    j["offset"] = s.offset ? Message(*s.offset) : Message(nullptr);
}

// --- Main engine ---

MainSync::MainSync(DocumentServer& document, Engine& engine, MediaLibrary& library, std::string name,
                   uint16_t clockPort, bool fresh)
    : document_(document), engine_(engine), library_(library), name_(std::move(name)), clockPort_(clockPort),
      fresh_(fresh),
      files_(dispatch_queue_create("nocturne.sync.files", DISPATCH_QUEUE_SERIAL)), thread_([this] { run(); })
{
}

MainSync::~MainSync()
{
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        stop_.notify_all();
    }
    thread_.join();
    dispatch_sync(files_, ^{});
}

size_t MainSync::nodes() const
{
    std::lock_guard lock(mutex_);
    return nodes_.size();
}

bool MainSync::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    if (op == "fetch") {
        fetch(message, reply);
        return true;
    }
    if (op != "attach")
        return false;
    const TransportState transport = engine_.transport();
    reply({{"type", "node"},
           {"op", "attached"},
           {"name", name_},
           {"clock", clockPort_},
           {"seed", engine_.seed()},
           {"fresh", fresh_ && document_.change() == 0},
           {"pilots", pilotsJson(transport.pilots)}});
    reply(beatMessage(transport.timeline));
    std::lock_guard lock(mutex_);
    nodes_.insert(message.value("client", ClientId(0)));
    return true;
}

void MainSync::fetch(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string hash = message.value("hash", "");
    const uint64_t offset = message.value("offset", uint64_t(0));
    const uint64_t size = std::min(message.value("size", kMaxChunk), kMaxChunk);
    MediaLibrary* library = &library_;
    DocumentServer::Sink sink = reply;
    dispatch_async(files_, ^{
        Message chunk = {{"type", "node"}, {"op", "chunk"}, {"hash", hash}, {"offset", offset}};
        const std::optional<MediaEntry> entry = library->find(hash);
        std::ifstream in;
        if (entry && entry->status == MediaStatus::Ok)
            in.open(entry->path, std::ios::binary);
        if (!in) {
            chunk["error"] = entry ? "cannot read " + entry->path.string() : "not in the media library";
            return sink(chunk);
        }
        in.seekg(0, std::ios::end);
        const uint64_t total = uint64_t(in.tellg());
        std::vector<uint8_t> data(size_t(offset < total ? std::min(size, total - offset) : 0));
        in.seekg(std::streamoff(offset));
        in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
        if (!in && !data.empty()) {
            chunk["error"] = "cannot read " + entry->path.string();
            return sink(chunk);
        }
        chunk["total"] = total;
        chunk["data"] = Message::binary(std::move(data));
        sink(chunk);
    });
}

void MainSync::run()
{
    std::optional<BeatTimeline> last;
    auto sent = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex_);
    while (!stop_.wait_for(lock, kBeatPoll, [&] { return stopped_; })) {
        if (nodes_.empty())
            continue;
        std::set<ClientId> nodes = nodes_;
        lock.unlock();
        const BeatTimeline timeline = engine_.transport().timeline;
        const auto now = std::chrono::steady_clock::now();
        std::set<ClientId> gone;
        if (timeline != last || now - sent >= kBeatHeartbeat) {
            last = timeline;
            sent = now;
            for (ClientId node : nodes)
                if (!document_.send(node, beatMessage(timeline)))
                    gone.insert(node);
        }
        lock.lock();
        for (ClientId node : gone)
            nodes_.erase(node);
    }
}

// --- Node ---

struct NodeSync::Session {
    struct Fetch {
        std::string hash;
        std::filesystem::path file;
        std::filesystem::path part;
        std::ofstream out;
        uint64_t total = 0;
        uint64_t received = 0;
        uint64_t requested = 0;
        int inFlight = 0;
    };

    Endpoint main;
    WebSocketClient* client = nullptr;
    std::unique_ptr<ClockLink> clock;
    DocumentMirror mirror;
    std::optional<BeatTimeline> beat;
    bool attached = false;
    bool held = false;
    std::chrono::steady_clock::time_point heard = std::chrono::steady_clock::now();
    bool closed = false;
    std::deque<std::string> wanted;
    std::set<std::string> unavailable;
    std::optional<Fetch> fetch;
};

NodeSync::NodeSync(DocumentServer& document, Engine& engine, MediaLibrary& library, Clock now, Locate locate,
                   ConnectClock clock, bool fetch)
    : document_(document), engine_(engine), library_(library), now_(std::move(now)), locate_(std::move(locate)),
      connectClock_(std::move(clock)), fetch_(fetch), files_(dispatch_queue_create("nocturne.sync.verify", DISPATCH_QUEUE_SERIAL))
{
    document_.setReadOnly(readOnlyReason(""), kNodeOpen);
    thread_ = std::thread([this] { run(); });
}

NodeSync::~NodeSync()
{
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
        wake_.notify_all();
    }
    thread_.join();
    dispatch_sync(files_, ^{});
}

NodeStatus NodeSync::status() const
{
    std::lock_guard lock(mutex_);
    NodeStatus status = status_;
    if (session_ && session_->clock) {
        status.offset = session_->clock->offsetAt(now_());
        status.roundTrip = session_->clock->roundTrip();
    }
    return status;
}

void NodeSync::run()
{
    while (true) {
        {
            std::lock_guard lock(mutex_);
            if (stopped_)
                return;
        }
        if (std::optional<Endpoint> main = locate_())
            session(*main);
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, kRetry, [&] { return stopped_; });
    }
}

// One connection to the main engine, until it closes, falls silent or the node stops.
void NodeSync::session(const Endpoint& main)
{
    std::unique_ptr<WebSocketClient> client;
    try {
        client = std::make_unique<WebSocketClient>(
            main.host, main.port, Encoding::MessagePack, [this](const Message& m) { receive(m); },
            [this] {
                std::lock_guard lock(mutex_);
                if (session_)
                    session_->closed = true;
                wake_.notify_all();
            });
    } catch (const std::exception& e) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        session_ = std::make_unique<Session>();
        session_->main = main;
        session_->client = client.get();
        status_.connected = true;
        status_.main = main.name.empty() ? main.host : main.name;
        document_.setReadOnly(readOnlyReason(status_.main), kNodeOpen);
        session_->client->send(msg::hello(main.token));
        session_->client->send({{"type", "node"}, {"op", "attach"}});
    }
    std::printf("node sync: following %s at %s:%u\n", main.name.c_str(), main.host.c_str(), unsigned(main.port));
    std::fflush(stdout);

    std::unique_ptr<Session> ended;
    {
        std::unique_lock lock(mutex_);
        while (!stopped_ && !session_->closed && std::chrono::steady_clock::now() - session_->heard < kTimeout) {
            wake_.wait_for(lock, kTick);
            follow();
        }
        abortFetch();
        session_->client = nullptr;
        ended = std::move(session_);
        status_.connected = false;
        status_.synced = false;
        status_.missing = 0;
    }
    std::printf("node sync: lost %s; playing on\n", main.name.c_str());
    std::fflush(stdout);
    client.reset();
    ended.reset();
}

// Under the lock.
void NodeSync::send(const Message& message)
{
    if (session_ && session_->client)
        session_->client->send(message);
}

void NodeSync::receive(const Message& message)
{
    std::lock_guard lock(mutex_);
    if (!session_ || !session_->client)
        return;
    session_->heard = std::chrono::steady_clock::now();
    try {
        const std::string type = message.value("type", "");
        if (type == "snapshot" || type == "change")
            return mirror(message);
        if (type == "beat") {
            session_->beat = beatFrom(message);
            return follow();
        }
        if (type == "clock" && message.value("op", "") == "scrub" && !session_->held)
            return engine_.scrub(message.at("layer").get<uint64_t>(), message.at("position").get<double>());
        if (type != "node")
            return;
        const std::string op = message.value("op", "");
        if (op == "attached" && !session_->attached)
            attached(message);
        else if (op == "chunk")
            chunk(message);
    } catch (const std::exception& e) {
        std::printf("node sync: %s\n", e.what());
        std::fflush(stdout);
    }
}

// Under the lock. Applies the main engine's change to this engine's document as the same edit, or replaces the
// document when that does not fit.
void NodeSync::mirror(const Message& message)
{
    const uint64_t before = session_->mirror.change();
    const bool wasSynced = session_->mirror.synced();
    if (std::optional<Message> request = session_->mirror.receive(message))
        send(*request);
    const DocumentMirror& mirror = session_->mirror;
    if (!mirror.synced() || !session_->attached)
        return;
    const bool snapshot = message["type"] == "snapshot";
    if (!snapshot && wasSynced && mirror.change() == before)
        return;
    if (session_->held) {
        if (!snapshot && message.value("op", "") == "loadComposition")
            release();
        return;
    }
    if (snapshot)
        return adoptMirror();
    try {
        document_.perform(editFromJson(message), "sync");
    } catch (const ProtocolError&) {
        return adoptMirror();
    }
    wantMissing();
}

// Under the lock: this engine's document becomes the main engine's.
void NodeSync::adoptMirror()
{
    const Composition& composition = session_->mirror.composition();
    if (Message(document_.document()) != Message(composition))
        document_.perform(LoadComposition{composition}, "sync");
    status_.synced = true;
    wantMissing();
}

// Under the lock. A main engine that runs the default composition it started with, untouched, has lost its show
// (restarted without state); a node holding a show keeps playing it rather than replacing it with nothing, until
// `node adopt` or until the main engine loads a composition.
void NodeSync::attached(const Message& message)
{
    session_->attached = true;
    if (message.contains("seed"))
        engine_.setSeed(message["seed"].get<uint32_t>());
    if (connectClock_)
        session_->clock = connectClock_(session_->main, message.value("clock", uint16_t(0)));
    session_->held = message.value("fresh", false) && hasShow(document_.document());
    status_.held = session_->held;
    if (session_->held) {
        std::printf("node sync: %s runs an empty, unedited composition; keeping this show (node adopt takes it)\n",
                    session_->main.name.c_str());
        std::fflush(stdout);
        return;
    }
    if (session_->mirror.synced())
        adoptMirror();
    engine_.adoptPilots(pilotsFrom(message.value("pilots", Message::array())));
}

// Under the lock.
void NodeSync::release()
{
    session_->held = false;
    status_.held = false;
    adoptMirror();
    follow();
}

void NodeSync::adopt()
{
    std::lock_guard lock(mutex_);
    if (session_ && session_->held && session_->mirror.synced())
        release();
}

// Under the lock: maps the main engine's beat timeline onto this engine's clock as of now.
void NodeSync::follow()
{
    if (!session_ || session_->held || !session_->beat || !session_->clock)
        return;
    const double local = now_();
    const std::optional<double> offset = session_->clock->offsetAt(local);
    if (!offset)
        return;
    const BeatTimeline& main = *session_->beat;
    engine_.follow({local, main.beatAt(local + *offset), main.bpm, main.nudge, main.resyncs});
}

// Under the lock.
void NodeSync::wantMissing()
{
    for (const auto& [hash, name] : mediaOf(session_->mirror.composition())) {
        if (have_.count(hash) || verifying_.count(hash) || session_->unavailable.count(hash) ||
            (session_->fetch && session_->fetch->hash == hash) ||
            std::find(session_->wanted.begin(), session_->wanted.end(), hash) != session_->wanted.end())
            continue;
        const std::optional<MediaEntry> entry = library_.find(hash);
        if (entry && entry->status != MediaStatus::Missing) {
            have_.insert(hash);
            continue;
        }
        session_->wanted.push_back(hash);
    }
    if (!session_->fetch)
        fetchNext();
    status_.missing = session_->wanted.size() + (session_->fetch ? 1 : 0);
}

// Under the lock.
void NodeSync::fetchNext()
{
    if (!fetch_)
        return;
    const std::map<std::string, std::string> media = mediaOf(session_->mirror.composition());
    while (!session_->wanted.empty()) {
        const std::string hash = session_->wanted.front();
        session_->wanted.pop_front();
        auto named = media.find(hash);
        if (named == media.end())
            continue;
        const std::filesystem::path folder = library_.root() / "media";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        Session::Fetch& fetch = session_->fetch.emplace();
        fetch.hash = hash;
        fetch.file = folder / (hash + std::filesystem::path(named->second).extension().string());
        fetch.part = fetch.file.string() + ".part";
        fetch.out.open(fetch.part, std::ios::binary | std::ios::trunc);
        if (!fetch.out) {
            session_->fetch.reset();
            session_->unavailable.insert(hash);
            continue;
        }
        status_.fetching = named->second;
        requestChunks();
        return;
    }
    status_.fetching.clear();
}

// Under the lock. The first chunk tells the size; then up to kWindow chunks are in flight.
void NodeSync::requestChunks()
{
    Session::Fetch& fetch = *session_->fetch;
    const bool sized = fetch.requested > 0 && fetch.received > 0;
    while (fetch.inFlight < kWindow && (fetch.requested == 0 || (sized && fetch.requested < fetch.total))) {
        send({{"type", "node"},
              {"op", "fetch"},
              {"hash", fetch.hash},
              {"offset", fetch.requested},
              {"size", MainSync::kMaxChunk}});
        fetch.requested += MainSync::kMaxChunk;
        fetch.inFlight++;
    }
}

// Under the lock.
void NodeSync::chunk(const Message& message)
{
    if (!session_->fetch || message.value("hash", "") != session_->fetch->hash)
        return;
    Session::Fetch& fetch = *session_->fetch;
    fetch.inFlight--;
    if (message.contains("error") || message.value("offset", uint64_t(0)) != fetch.received) {
        std::printf("node sync: cannot copy %s: %s\n", fetch.hash.c_str(), message.value("error", "out of order").c_str());
        std::fflush(stdout);
        session_->unavailable.insert(fetch.hash);
        abortFetch();
        return fetchNext();
    }
    const auto& data = message.at("data").get_binary();
    fetch.out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    fetch.received += data.size();
    fetch.total = message.at("total").get<uint64_t>();
    status_.bytes += data.size();
    if (fetch.received < fetch.total)
        return requestChunks();
    fetch.out.close();
    const std::string hash = fetch.hash;
    verifying_.insert(hash);
    verify(hash, fetch.part, fetch.file);
    session_->fetch.reset();
    fetchNext();
    status_.missing = session_->wanted.size() + (session_->fetch ? 1 : 0);
}

// Under the lock.
void NodeSync::abortFetch()
{
    if (!session_ || !session_->fetch)
        return;
    session_->fetch->out.close();
    std::error_code ec;
    std::filesystem::remove(session_->fetch->part, ec);
    session_->fetch.reset();
    status_.fetching.clear();
}

// Hashes the copy off the connection's queue: a gigabyte takes seconds.
void NodeSync::verify(std::string hash, std::filesystem::path part, std::filesystem::path file)
{
    dispatch_async(files_, ^{
        bool ok = false;
        std::error_code ec;
        std::filesystem::rename(part, file, ec);
        try {
            ok = !ec && library_.add(file).hash == hash;
        } catch (const std::exception&) {
        }
        if (!ok)
            std::filesystem::remove(file, ec);
        if (ok)
            engine_.mediaAdded(hash);
        std::printf("node sync: %s %s\n", ok ? "copied" : "copy failed for", file.filename().c_str());
        std::fflush(stdout);
        std::lock_guard lock(mutex_);
        verifying_.erase(hash);
        if (ok) {
            have_.insert(hash);
            status_.files++;
        } else if (session_) {
            session_->unavailable.insert(hash);
        }
    });
}

// --- Role ---

SyncRole::SyncRole(DocumentServer& document, Engine& engine, MediaLibrary& library, Clock now, Options options)
    : document_(document), engine_(engine), library_(library), now_(std::move(now)), options_(std::move(options)),
      role_(options_.role)
{
    become(role_, options_.fresh);
    document_.handle("node", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

SyncRole::~SyncRole()
{
    document_.handle("node", nullptr);
    std::lock_guard lock(mutex_);
    node_.reset();
    main_.reset();
}

Role SyncRole::role() const
{
    std::lock_guard lock(mutex_);
    return role_;
}

void SyncRole::promote()
{
    {
        std::lock_guard lock(mutex_);
        if (role_ == Role::Main)
            return;
        become(Role::Main, false);
    }
    if (options_.changed)
        options_.changed(Role::Main);
}

void SyncRole::demote()
{
    {
        std::lock_guard lock(mutex_);
        if (role_ == Role::Node)
            return;
        become(Role::Node, false);
    }
    if (options_.changed)
        options_.changed(Role::Node);
}

// Under the lock (or in the constructor).
void SyncRole::become(Role role, bool fresh)
{
    role_ = role;
    if (role == Role::Main) {
        node_.reset();
        engine_.unfollow();
        document_.setReadOnly(std::nullopt);
        main_ = std::make_unique<MainSync>(document_, engine_, library_, options_.name, options_.clockPort, fresh);
        std::printf("node sync: main engine\n");
    } else {
        main_.reset();
        node_ = std::make_unique<NodeSync>(document_, engine_, library_, now_, options_.locate, options_.clock);
        std::printf("node sync: node\n");
    }
    std::fflush(stdout);
}

Message SyncRole::status() const
{
    std::lock_guard lock(mutex_);
    Message status = {{"type", "node"}, {"op", "status"}, {"role", role_ == Role::Main ? "main" : "node"},
                      {"name", options_.name}};
    if (main_)
        status["nodes"] = main_->nodes();
    if (node_)
        status["node"] = node_->status();
    return status;
}

void SyncRole::adopt()
{
    std::lock_guard lock(mutex_);
    if (node_)
        node_->adopt();
}

void SyncRole::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    if (op == "promote")
        promote();
    else if (op == "adopt")
        adopt();
    else if (op == "demote")
        demote();
    else if (op != "status") {
        std::lock_guard lock(mutex_);
        if (!main_ || !main_->handle(message, reply))
            reply(error(main_ ? "unknown node op " + op : "not the main engine"));
        return;
    }
    Message answer = status();
    answer["op"] = op;
    reply(answer);
}

} // namespace azl
