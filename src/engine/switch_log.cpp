#include "engine/switch_log.hpp"

#include "document/lineup.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace azl {

namespace {

constexpr auto kSettleWait = std::chrono::seconds(2);

std::string deckName(const Composition& composition, uint64_t deck)
{
    for (const Deck& d : composition.decks)
        if (d.id == deck)
            return d.name;
    return "";
}

} // namespace

SwitchLog::SwitchLog(DocumentServer& document, Options options)
    : document_(document), options_(std::move(options)), deck_(document.document().deck)
{
    thread_ = std::thread([this] { run(); });
    client_ = document_.connect([this](const Message& message) { changed(message); });
    document_.receive(client_, msg::hello());
    document_.handle("switches", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
}

SwitchLog::~SwitchLog()
{
    document_.handle("switches", nullptr);
    document_.disconnect(client_);
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

std::filesystem::path SwitchLog::file(const std::filesystem::path& folder, const std::string& date)
{
    return folder / "Sets" / date / "switches.jsonl";
}

std::vector<Message> SwitchLog::read(const std::filesystem::path& file)
{
    std::vector<Message> lines;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        Message parsed = Message::parse(line, nullptr, false);
        if (parsed.is_object())
            lines.push_back(std::move(parsed));
    }
    return lines;
}

// Runs under the document's lock: only notes the switch.
void SwitchLog::changed(const Message& message)
{
    if (message.value("type", "") != "change")
        return;
    const std::string op = message.value("op", "");
    if (op == "loadComposition") {
        deck_ = message["composition"].value("deck", deck_);
        return;
    }
    if (op != "selectDeck" || message.value("deck", deck_) == deck_)
        return;
    deck_ = message["deck"].get<uint64_t>();
    Switch s{options_.wall(), options_.now(), deck_, message.value("cause", "")};
    std::lock_guard lock(mutex_);
    queue_.push_back(std::move(s));
    wake_.notify_all();
}

void SwitchLog::waitIdle()
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

void SwitchLog::run()
{
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (stop_)
            return;
        const Switch s = queue_.front();
        queue_.pop_front();
        busy_ = true;
        lock.unlock();
        write(s);
        lock.lock();
        busy_ = false;
        idle_.notify_all();
    }
}

// Waits a little for the recorder to pass the switch, so a rollover right at it cannot move it to the other file.
Message SwitchLog::recording(double time)
{
    if (!options_.recorder)
        return nullptr;
    const auto until = std::chrono::steady_clock::now() + kSettleWait;
    std::optional<Recorder::Position> position = options_.recorder->position(time);
    while (position && !position->settled && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        {
            std::lock_guard lock(mutex_);
            if (stop_)
                break;
        }
        if (std::optional<Recorder::Position> later = options_.recorder->position(time))
            position = later;
        else
            break;
    }
    if (!position)
        return nullptr;
    return {{"file", position->file.string()}, {"mediaTime", std::round(position->mediaTime * 1e6) / 1e6}};
}

void SwitchLog::write(const Switch& s)
{
    const Composition composition = document_.document();
    const std::optional<Night> night = nightAt(composition, s.wall, options_.zone);
    const LineupSlot* slot = night ? slotFor(*night, s.deck, s.wall) : nullptr;
    const std::string date = nightDate(composition, s.wall, options_.zone);
    const Message line{{"utc", isoTime(s.wall)},
                       {"local", isoTime(s.wall, options_.zone(int64_t(std::floor(s.wall))))},
                       {"deck", s.deck},
                       {"deckName", deckName(composition, s.deck)},
                       {"dj", slot ? Message(slot->dj) : Message(nullptr)},
                       {"slot", slot ? Message(slot->slot) : Message(nullptr)},
                       {"cause", s.cause},
                       {"recording", recording(s.time)}};
    const std::filesystem::path path = file(options_.folder(), date);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::app);
    out << line.dump() << '\n';
    out.flush();
    if (!out) {
        if (!failing_)
            std::printf("switches: cannot write %s; deck switches go unlogged until it can\n", path.string().c_str());
        failing_ = true;
        return;
    }
    failing_ = false;
    const Message added{{"type", "switches"}, {"op", "added"}, {"date", date}, {"switch", line}};
    std::lock_guard lock(watchersMutex_);
    std::erase_if(watchers_, [&](ClientId watcher) { return !document_.send(watcher, added); });
}

void SwitchLog::handle(const Message& message, const DocumentServer::Sink& reply)
{
    try {
        if (message.value("op", "") != "list")
            throw ProtocolError("unknown switches op");
        std::string date = message.value("date", "");
        if (date.empty())
            date = nightDate(document_.document(), options_.wall(), options_.zone);
        else if (!dateDays(date))
            throw ProtocolError("bad date " + date);
        Message lines = Message::array();
        for (Message& line : read(file(options_.folder(), date)))
            lines.push_back(std::move(line));
        reply({{"type", "switches"}, {"op", "list"}, {"date", date}, {"switches", lines}});
        if (message.value("watch", false)) {
            std::lock_guard lock(watchersMutex_);
            watchers_.insert(message.value("client", ClientId(0)));
        }
    } catch (const std::exception& e) {
        reply({{"type", "error"}, {"message", e.what()}});
    }
}

} // namespace azl
