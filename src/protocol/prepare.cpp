#include "protocol/prepare.hpp"

#include "protocol/document_server.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>

namespace azl {

namespace {

using nlohmann::json;

void dropPlayback(json& j)
{
    if (j.is_object()) {
        j.erase("playing");
        j.erase("armed");
        if (j.contains("clips"))
            j.erase("clip");
        for (auto& [key, value] : j.items())
            dropPlayback(value);
    } else if (j.is_array()) {
        for (json& value : j)
            dropPlayback(value);
    }
}

// The elements of an array of objects with distinct ids, by id; none for any other value.
std::optional<std::map<uint64_t, const json*>> byId(const json& array)
{
    if (!array.is_array())
        return std::nullopt;
    std::map<uint64_t, const json*> elements;
    for (const json& element : array) {
        auto id = element.is_object() ? element.find("id") : element.end();
        if (!element.is_object() || id == element.end() || !id->is_number_unsigned() ||
            !elements.emplace(id->get<uint64_t>(), &element).second)
            return std::nullopt;
    }
    return elements;
}

std::vector<uint64_t> idOrder(const json& array, const std::map<uint64_t, const json*>& keep)
{
    std::vector<uint64_t> ids;
    for (const json& element : array)
        if (const uint64_t id = element["id"].get<uint64_t>(); keep.contains(id))
            ids.push_back(id);
    return ids;
}

std::string idKey(uint64_t id)
{
    return "#" + std::to_string(id);
}

void diff(const json& a, const json& b, DocPath& at, std::vector<DocPath>& out)
{
    if (a == b)
        return;
    auto mark = [&](const std::string& key) {
        at.push_back(key);
        out.push_back(at);
        at.pop_back();
    };
    if (a.is_object() && b.is_object()) {
        for (const auto& [key, value] : a.items()) {
            auto other = b.find(key);
            if (other == b.end()) {
                mark(key);
                continue;
            }
            at.push_back(key);
            diff(value, *other, at, out);
            at.pop_back();
        }
        for (const auto& [key, value] : b.items())
            if (!a.contains(key))
                mark(key);
        return;
    }
    const auto before = byId(a);
    const auto after = byId(b);
    if (!before || !after) {
        out.push_back(at);
        return;
    }
    for (const auto& [id, element] : *before) {
        auto other = after->find(id);
        if (other == after->end()) {
            mark(idKey(id));
            continue;
        }
        at.push_back(idKey(id));
        diff(*element, *other->second, at, out);
        at.pop_back();
    }
    for (const auto& [id, element] : *after)
        if (!before->contains(id))
            mark(idKey(id));
    if (idOrder(a, *after) != idOrder(b, *before))
        mark("@order");
}

std::optional<json> resolve(const json& root, const DocPath& path)
{
    const json* at = &root;
    for (size_t i = 0; i < path.size(); ++i) {
        const std::string& key = path[i];
        if (at->is_object()) {
            auto it = at->find(key);
            if (it == at->end())
                return std::nullopt;
            at = &*it;
            continue;
        }
        const auto elements = byId(*at);
        if (!elements)
            return std::nullopt;
        if (key == "@order")
            return i + 1 == path.size() ? std::optional<json>(json(idOrder(*at, *elements))) : std::nullopt;
        if (key.size() < 2 || key[0] != '#')
            return std::nullopt;
        auto it = elements->find(std::strtoull(key.c_str() + 1, nullptr, 10));
        if (it == elements->end())
            return std::nullopt;
        at = it->second;
    }
    return *at;
}

std::string pathText(const DocPath& path)
{
    std::string text;
    for (const std::string& key : path)
        text += (text.empty() ? "" : "/") + key;
    return text;
}

std::string opOf(const Edit& edit)
{
    return editToJson(edit).value("op", "");
}

Rebased rebaseOn(const Composition& venue, const json& now, const std::vector<PreparedEdit>& edits,
                 bool keepDeck = false)
{
    Rebased rebased{venue, {}, {}};
    for (const PreparedEdit& prepared : edits) {
        auto changed = std::find_if(prepared.touched.begin(), prepared.touched.end(),
                                    [&](const Touch& t) { return resolve(now, t.path) != t.base; });
        std::string why = "the venue changed it meanwhile";
        if (changed == prepared.touched.end()) {
            Edit edit = prepared.edit;
            try {
                const uint64_t deck = rebased.composition.deck;
                Edit inverse = applyEdit(rebased.composition, edit);
                if (keepDeck && rebased.composition.deck != deck) {
                    applyEdit(rebased.composition, inverse);
                    throw ProtocolError("it would switch the deck shown");
                }
                rebased.applied.push_back(std::move(edit));
                continue;
            } catch (const std::exception& e) {
                why = e.what();
            }
        }
        std::string where;
        if (changed != prepared.touched.end())
            where = pathText(changed->path);
        else if (!prepared.touched.empty())
            where = pathText(prepared.touched.front().path);
        rebased.skipped.push_back(opOf(prepared.edit) + (where.empty() ? "" : " (" + where + ")") + ": " + why);
    }
    return rebased;
}

} // namespace

json documentJson(const Composition& composition)
{
    json j = composition;
    for (const char* key : {"deck", "column", "timeline", "palette", "palettePlaying"})
        j.erase(key);
    if (auto cues = j.find("cues"); cues != j.end())
        cues->erase(std::remove_if(cues->begin(), cues->end(), [](const json& cue) { return cue.contains("prepared"); }),
                    cues->end());
    dropPlayback(j);
    return j;
}

Rebased rebase(const Composition& venue, const std::vector<PreparedEdit>& edits, bool keepDeck)
{
    if (edits.empty())
        return {venue, {}, {}};
    return rebaseOn(venue, documentJson(venue), edits, keepDeck);
}

json preparedToJson(const std::vector<PreparedEdit>& edits)
{
    json list = json::array();
    for (const PreparedEdit& prepared : edits) {
        json entry = editToJson(prepared.edit);
        json touched = json::array();
        for (const Touch& t : prepared.touched) {
            json touch{{"path", t.path}};
            if (t.base)
                touch["base"] = *t.base;
            touched.push_back(std::move(touch));
        }
        entry["touched"] = std::move(touched);
        list.push_back(std::move(entry));
    }
    return list;
}

std::vector<PreparedEdit> preparedFromJson(const json& j)
{
    if (!j.is_array())
        throw ProtocolError("prepared edits are a list");
    std::vector<PreparedEdit> edits;
    for (const json& entry : j) {
        PreparedEdit prepared{editFromJson(entry), {}};
        if (isLive(prepared.edit))
            throw ProtocolError(opOf(prepared.edit) + " is a live action");
        const json touched = entry.value("touched", json::array());
        if (!touched.is_array())
            throw ProtocolError("bad field touched");
        for (const json& t : touched) {
            if (!t.is_object() || !t.contains("path") || !t["path"].is_array())
                throw ProtocolError("bad field touched");
            Touch touch{{}, std::nullopt};
            for (const json& key : t["path"]) {
                if (!key.is_string())
                    throw ProtocolError("bad field touched");
                touch.path.push_back(key.get<std::string>());
            }
            if (auto base = t.find("base"); base != t.end())
                touch.base = *base;
            prepared.touched.push_back(std::move(touch));
        }
        edits.push_back(std::move(prepared));
    }
    return edits;
}

PreparedCopy::PreparedCopy(const Composition& venue)
{
    follow(venue);
}

void PreparedCopy::follow(const Composition& venue)
{
    venue_ = venue;
    venueJson_ = documentJson(venue_);
    refresh();
}

void PreparedCopy::refresh()
{
    copy_ = rebaseOn(venue_, venueJson_, edits());
    copyJson_ = documentJson(copy_.composition);
}

void PreparedCopy::edit(Edit edit, uint64_t gesture)
{
    if (isLive(edit))
        throw ProtocolError(opOf(edit) + " is a live action");
    Composition after = copy_.composition;
    applyEdit(after, edit);
    json afterJson = documentJson(after);
    std::vector<DocPath> paths;
    DocPath at;
    diff(copyJson_, afterJson, at, paths);
    PreparedEdit prepared{std::move(edit), {}};
    for (DocPath& path : paths) {
        std::optional<json> base = resolve(venueJson_, path);
        prepared.touched.push_back({std::move(path), std::move(base)});
    }
    copy_.applied.push_back(prepared.edit);
    copy_.composition = std::move(after);
    copyJson_ = std::move(afterJson);
    redo_.clear();
    if (!gesture || undo_.empty() || undo_.back().gesture != gesture) {
        undo_.push_back({{std::move(prepared)}, gesture});
        return;
    }
    std::vector<PreparedEdit>& step = undo_.back().edits;
    if (!sameEdit(step.back().edit, prepared.edit)) {
        step.push_back(std::move(prepared));
        return;
    }
    PreparedEdit& last = step.back();
    last.edit = std::move(prepared.edit);
    for (Touch& touch : prepared.touched)
        if (std::none_of(last.touched.begin(), last.touched.end(), [&](const Touch& t) { return t.path == touch.path; }))
            last.touched.push_back(std::move(touch));
}

bool PreparedCopy::undo()
{
    if (undo_.empty())
        return false;
    redo_.push_back(std::move(undo_.back()));
    redo_.back().gesture = 0;
    undo_.pop_back();
    refresh();
    return true;
}

bool PreparedCopy::redo()
{
    if (redo_.empty())
        return false;
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    refresh();
    return true;
}

std::vector<PreparedEdit> PreparedCopy::edits() const
{
    std::vector<PreparedEdit> all;
    for (const Step& step : undo_)
        all.insert(all.end(), step.edits.begin(), step.edits.end());
    return all;
}

std::vector<std::string> PreparedCopy::ops(bool undo) const
{
    std::vector<std::string> list;
    for (const Step& step : undo ? undo_ : redo_)
        list.push_back(opOf(step.edits.front().edit));
    return list;
}

json PreparedCopy::toJson() const
{
    auto steps = [](const std::vector<Step>& from) {
        json list = json::array();
        for (const Step& step : from)
            list.push_back(preparedToJson(step.edits));
        return list;
    };
    return {{"undo", steps(undo_)}, {"redo", steps(redo_)}};
}

PreparedCopy PreparedCopy::fromJson(const json& j, const Composition& venue)
{
    PreparedCopy copy(venue);
    auto steps = [&](const char* key, std::vector<Step>& to) {
        const json list = j.is_object() ? j.value(key, json::array()) : json();
        if (!list.is_array())
            throw ProtocolError("prepared steps are a list");
        for (const json& step : list)
            to.push_back({preparedFromJson(step), 0});
    };
    steps("undo", copy.undo_);
    steps("redo", copy.redo_);
    copy.refresh();
    return copy;
}

} // namespace azl
