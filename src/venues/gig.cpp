#include "venues/gig.hpp"

#include "document/cue_edits.hpp"
#include "platform/app_paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace azl {

namespace fs = std::filesystem;

namespace {

constexpr int kDayMinutes = 24 * 60;

int minutesField(const nlohmann::json& j, const char* key)
{
    const auto it = j.find(key);
    const std::optional<int> minutes = it != j.end() && it->is_string() ? clockMinutes(it->get<std::string>()) : std::nullopt;
    if (!minutes)
        throw ProtocolError(std::string("a Gig's ") + key + " reads HH:MM");
    return *minutes;
}

std::string textField(const nlohmann::json& j, const char* key)
{
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        throw ProtocolError(std::string("a Gig needs ") + key);
    return it->get<std::string>();
}

// A venue name as one folder name.
std::string folderName(const std::string& venue)
{
    std::string name = venue.empty() ? "venue" : venue;
    std::replace(name.begin(), name.end(), '/', '_');
    return name == "." || name == ".." ? "_" + name : name;
}

int number(const std::string& word, const char* what)
{
    try {
        size_t used = 0;
        const int value = std::stoi(word, &used);
        if (used == word.size())
            return value;
    } catch (const std::exception&) {
    }
    throw std::runtime_error(std::string("bad ") + what + " " + word);
}

int clockWord(const std::string& word)
{
    const std::optional<int> minutes = clockMinutes(word);
    if (!minutes)
        throw std::runtime_error("bad time " + word + ", it reads HH:MM");
    return *minutes;
}

bool isDate(const std::string& word)
{
    return dateDays(word).has_value();
}

// DJ@HH:MM=DECK; the DJ's name may hold anything but its last @.
GigSlot slotWord(const std::string& word)
{
    const size_t at = word.rfind('@');
    const size_t equals = word.rfind('=');
    if (at == std::string::npos || at == 0 || equals == std::string::npos || equals < at)
        throw std::runtime_error("bad slot " + word + ", it reads DJ@HH:MM=DECK");
    return {word.substr(0, at), clockWord(word.substr(at + 1, equals - at - 1)),
            uint64_t(number(word.substr(equals + 1), "deck"))};
}

} // namespace

void to_json(nlohmann::json& j, const Gig& gig)
{
    nlohmann::json lineup = nlohmann::json::array();
    for (const GigSlot& slot : gig.lineup)
        lineup.push_back({{"dj", slot.dj}, {"start", clockText(slot.start)}, {"deck", slot.deck}});
    j = {{"schema", kGigSchema},
         {"venue", gig.venue},
         {"date", gig.date},
         {"window", {{"start", clockText(gig.start)}, {"end", clockText(gig.end)}}},
         {"lineup", lineup}};
}

void from_json(const nlohmann::json& j, Gig& gig)
{
    if (!j.is_object())
        throw ProtocolError("a Gig is an object");
    if (j.value("schema", kGigSchema) != kGigSchema)
        throw ProtocolError("a Gig of schema " + j["schema"].dump() + " is newer than this build");
    gig = Gig{};
    gig.venue = j.contains("venue") ? textField(j, "venue") : "";
    gig.date = textField(j, "date");
    const auto window = j.find("window");
    if (window == j.end() || !window->is_object())
        throw ProtocolError("a Gig needs a window");
    gig.start = minutesField(*window, "start");
    gig.end = minutesField(*window, "end");
    for (const nlohmann::json& slot : j.value("lineup", nlohmann::json::array())) {
        if (!slot.is_object() || !slot.contains("deck") || !slot["deck"].is_number_unsigned())
            throw ProtocolError("a lineup slot needs a dj, a start and a deck");
        gig.lineup.push_back({textField(slot, "dj"), minutesField(slot, "start"), slot["deck"].get<uint64_t>()});
    }
    checkGig(gig);
}

void checkGig(const Gig& gig)
{
    if (!dateDays(gig.date))
        throw ProtocolError("a Gig's date reads YYYY-MM-DD");
    if (gig.start < 0 || gig.start >= kDayMinutes || gig.end < 0 || gig.end >= kDayMinutes || gig.start == gig.end)
        throw ProtocolError("a Gig's window starts and ends at different times");
    for (const GigSlot& slot : gig.lineup) {
        if (slot.dj.empty())
            throw ProtocolError("a lineup slot needs a DJ");
        if (slot.start < 0 || slot.start >= kDayMinutes)
            throw ProtocolError("a lineup slot starts within 00:00..23:59");
        if (!slot.deck)
            throw ProtocolError("a lineup slot needs a deck");
    }
}

fs::path defaultGigsFolder()
{
    return appDataFolder() / "Gigs";
}

fs::path gigPath(const fs::path& folder, const std::string& venue, const std::string& date)
{
    return folder / folderName(venue) / (date + ".json");
}

Gig loadGig(const fs::path& path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());
    try {
        return nlohmann::json::parse(in).get<Gig>();
    } catch (const nlohmann::json::exception& e) {
        throw ProtocolError(path.string() + ": " + e.what());
    }
}

void saveGig(const fs::path& path, const Gig& gig)
{
    checkGig(gig);
    fs::create_directories(path.parent_path());
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << nlohmann::json(gig).dump(2) << '\n';
        if (!out)
            throw std::runtime_error("cannot write " + temporary.string());
    }
    fs::rename(temporary, path);
}

std::vector<Gig> venueGigs(const fs::path& folder, const std::string& venue)
{
    std::vector<Gig> gigs;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder / folderName(venue), error)) {
        if (entry.path().extension() != ".json")
            continue;
        try {
            gigs.push_back(loadGig(entry.path()));
        } catch (const std::exception&) {
        }
    }
    std::sort(gigs.begin(), gigs.end(), [](const Gig& a, const Gig& b) { return a.date < b.date; });
    return gigs;
}

std::optional<Gig> currentGig(const std::vector<Gig>& gigs, double now, const UtcOffset& zone)
{
    for (const Gig& gig : gigs)
        if (double(gigSpan(gigWindow(gig), zone).second) > now)
            return gig;
    return std::nullopt;
}

std::string slotDate(const Gig& gig, int minute)
{
    const std::optional<int64_t> day = dateDays(gig.date);
    if (!day)
        return gig.date;
    return dateText(minute < gig.start ? *day + 1 : *day);
}

GigWindow gigWindow(const Gig& gig)
{
    return {gig.date, gig.start, gig.end};
}

std::vector<Cue> lineupCues(const Gig& gig)
{
    std::vector<Cue> cues;
    for (const GigSlot& slot : gig.lineup)
        cues.push_back({.name = slot.dj,
                        .column = CueColumn{0, 0},
                        .at = CueAt{slot.start, slotDate(gig, slot.start)},
                        .deck = slot.deck,
                        .gig = gig.date});
    return cues;
}

Edit gigEdit(const Gig& gig, const Composition& composition)
{
    checkGig(gig);
    Batch batch{{SetGigWindow{gig.date, gigWindow(gig)}}};
    for (const Cue& cue : composition.cues)
        if (cue.gig == gig.date)
            batch.edits.push_back(RemoveCue{cue.id});
    for (const Cue& cue : lineupCues(gig))
        batch.edits.push_back(AddCue{std::nullopt, cue});
    return batch;
}

void shiftSlot(Gig& gig, size_t slot, int minutes, bool following)
{
    if (slot >= gig.lineup.size())
        throw std::runtime_error("no slot " + std::to_string(slot + 1) + " in the lineup");
    for (size_t i = slot; i < (following ? gig.lineup.size() : slot + 1); ++i)
        gig.lineup[i].start = ((gig.lineup[i].start + minutes) % kDayMinutes + kDayMinutes) % kDayMinutes;
}

Cue cueAtWords(const std::vector<std::string>& words)
{
    if (words.size() < 4 || words[0] != "cue" || words[1] != "at")
        throw std::runtime_error("cue at HH:MM NAME [date:YYYY-MM-DD] [deck:DECK] [column:COLUMN|column:#N]");
    Cue cue{.name = words[3], .at = CueAt{clockWord(words[2]), std::nullopt}};
    for (size_t i = 4; i < words.size(); ++i) {
        const std::string& word = words[i];
        const size_t colon = word.find(':');
        const std::string kind = word.substr(0, colon);
        const std::string value = colon == std::string::npos ? "" : word.substr(colon + 1);
        if (kind == "date" && isDate(value))
            cue.at->date = value;
        else if (kind == "deck" && !value.empty())
            cue.deck = uint64_t(number(value, "deck"));
        else if (kind == "column" && value.starts_with("#"))
            cue.column = CueColumn{0, size_t(number(value.substr(1), "column") - 1)};
        else if (kind == "column" && !value.empty())
            cue.column = CueColumn{uint64_t(number(value, "column")), std::nullopt};
        else
            throw std::runtime_error("bad cue change " + word);
    }
    return cue;
}

GigCommand gigCommandWords(const std::vector<std::string>& words)
{
    if (words.size() < 2 || words[0] != "gig")
        throw std::runtime_error("gig show|apply|set|shift");
    GigCommand command{words[1]};
    const auto dateAt = [&](size_t i) {
        if (!isDate(words[i]))
            throw std::runtime_error("bad date " + words[i] + ", it reads YYYY-MM-DD");
        command.date = words[i];
    };
    if ((command.op == "show" || command.op == "apply") && words.size() <= 3) {
        if (words.size() == 3)
            dateAt(2);
        return command;
    }
    if (command.op == "set" && words.size() >= 4) {
        dateAt(2);
        const size_t dash = words[3].find('-');
        if (dash == std::string::npos)
            throw std::runtime_error("bad window " + words[3] + ", it reads HH:MM-HH:MM");
        command.gig = {"", words[2], clockWord(words[3].substr(0, dash)), clockWord(words[3].substr(dash + 1)), {}};
        for (size_t i = 4; i < words.size(); ++i)
            command.gig.lineup.push_back(slotWord(words[i]));
        try {
            checkGig(command.gig);
        } catch (const ProtocolError& e) {
            throw std::runtime_error(e.what());
        }
        return command;
    }
    if (command.op == "shift" && words.size() >= 4 && words.size() <= 6) {
        const int slot = number(words[2], "slot");
        if (slot < 1)
            throw std::runtime_error("slots count from 1");
        command.slot = size_t(slot - 1);
        command.minutes = number(words[3], "minutes");
        for (size_t i = 4; i < words.size(); ++i) {
            if (words[i] == "following")
                command.following = true;
            else
                dateAt(i);
        }
        return command;
    }
    throw std::runtime_error("gig show [DATE] | apply [DATE] | set DATE HH:MM-HH:MM [DJ@HH:MM=DECK ...] | "
                             "shift SLOT MINUTES [following] [DATE]");
}

} // namespace azl
