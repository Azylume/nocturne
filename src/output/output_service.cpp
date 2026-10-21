#include "output/output_service.hpp"

#include "output/display_keeper.hpp"

#include <cstdio>
#include <utility>

namespace azl {

namespace {

std::set<std::string> idsOf(const std::vector<DisplayInfo>& displays)
{
    std::set<std::string> ids;
    for (const DisplayInfo& d : displays)
        ids.insert(d.id);
    return ids;
}

void log(const std::string& text)
{
    std::printf("outputs: %s\n", text.c_str());
    std::fflush(stdout);
}

std::string screenName(const Composition& composition, uint64_t id)
{
    for (const Screen& screen : composition.screens)
        if (screen.id == id)
            return screen.name.empty() ? "Screen " + std::to_string(id) : screen.name;
    return "Screen " + std::to_string(id);
}

} // namespace

OutputService::OutputService(DocumentServer& document, Displays& displays, Outputs& outputs, KnownDisplays& known,
                             const ScreenDisplays& screens, Post post)
    : document_(document), displays_(displays), outputs_(outputs), known_(known), screens_(screens),
      post_(std::move(post))
{
    document_.handle("outputs", [this](const Message& message, DocumentServer::Sink reply) { handle(message, reply); });
    outputs_.onDisplaysChanged([this] { keep(); });
}

OutputService::~OutputService()
{
    outputs_.onDisplaysChanged(nullptr);
    document_.handle("outputs", nullptr);
}

// Unmirroring reconfigures the displays, which calls back in here; the outer call has the whole picture already.
void OutputService::keep()
{
    if (keeping_)
        return;
    keeping_ = true;
    rebindReturned();
    endMirroring(false);
    keeping_ = false;
    tell(list());
}

int OutputService::unmirrorNow()
{
    if (keeping_)
        return 0;
    keeping_ = true;
    const int ended = endMirroring(true);
    keeping_ = false;
    keep();
    return ended;
}

void OutputService::compositionChanged(const Composition& composition)
{
    std::vector<std::string> displays;
    for (const Screen& screen : composition.screens)
        displays.push_back(screen.display);
    if (displays == screenDisplays_)
        return;
    screenDisplays_ = std::move(displays);
    keep();
}

void OutputService::rebindReturned()
{
    const std::vector<DisplayInfo> attached = displays_.attached();
    const std::set<std::string> before = std::exchange(seen_, idsOf(attached));
    const Composition composition = document_.document();
    for (const DisplayRebind& r : returnedDisplays(composition, attached, before, known_)) {
        try {
            document_.perform(SetScreenDisplay{r.screen, r.to}, "displayReturned");
        } catch (const std::exception& e) {
            log("cannot move screen " + std::to_string(r.screen) + " to display " + r.to + ": " + e.what());
            continue;
        }
        notice({{"kind", "rebound"},
                {"screen", r.screen},
                {"from", r.from},
                {"to", r.to},
                {"text", screenName(composition, r.screen) + " took back its display " + displayName(r.from) +
                             ", which came back as " + r.to + " (was " + r.from + ")"}});
    }
}

int OutputService::endMirroring(bool always)
{
    if (!always && !known_.unmirror())
        return 0;
    int ended = 0;
    for (const std::vector<std::string>& mirrored : mirrorsAcrossScreens(document_.document(), displays_.attached())) {
        std::string names;
        for (const std::string& id : mirrored)
            names += (names.empty() ? "" : ", ") + displayName(id);
        if (!displays_.unmirror(mirrored)) {
            log("macOS refused to end the mirroring of " + names);
            continue;
        }
        ++ended;
        notice({{"kind", "unmirrored"},
                {"displays", mirrored},
                {"text", "Display mirroring turned off: " + names + " carry different screens"}});
    }
    return ended;
}

void OutputService::notice(Message notice)
{
    log(notice.value("text", ""));
    notice["type"] = "outputs";
    notice["op"] = "notice";
    tell(notice);
}

void OutputService::tell(const Message& message)
{
    std::lock_guard lock(mutex_);
    std::erase_if(listeners_, [&](ClientId client) { return !document_.send(client, message); });
}

std::string OutputService::displayName(const std::string& id) const
{
    for (const DisplayInfo& d : displays_.attached())
        if (d.id == id && !d.name.empty())
            return d.name;
    const std::optional<DisplayInfo> known = known_.find(id);
    return known && !known->name.empty() ? known->name : id;
}

void OutputService::handle(const Message& message, const DocumentServer::Sink& reply)
{
    if (const ClientId client = message.value("client", ClientId(0))) {
        std::lock_guard lock(mutex_);
        listeners_.insert(client);
    }
    const std::string op = message.value("op", "");
    if (op == "list")
        return post_([this, reply] { reply(list()); });
    if (op == "main" && message.contains("display") && message["display"].is_string())
        return post_([this, reply, display = message["display"].get<std::string>()] {
            outputs_.setMainDisplay(display);
            reply(list());
        });
    if (op == "guides" && message.contains("show") && message["show"].is_boolean())
        return post_([this, reply, show = message["show"].get<bool>()] {
            outputs_.setGuides(show);
            reply(list());
        });
    if (op == "target" && message.contains("display") && message["display"].is_string()) {
        std::optional<Point> point;
        try {
            if (!message.value("point", Message()).is_null())
                point = message["point"].get<Point>();
        } catch (const nlohmann::json::exception&) {
            return reply({{"type", "error"}, {"message", "bad point"}});
        }
        return post_([this, reply, point, display = message["display"].get<std::string>()] {
            outputs_.setTarget(display, point);
            reply(list());
        });
    }
    if (op == "identify") {
        const std::string display = message.value("display", Message()).is_string() ? message["display"].get<std::string>() : "";
        const double seconds = message.value("seconds", Message()).is_number() ? message["seconds"].get<double>() : 3.0;
        return post_([this, reply, display, seconds] {
            outputs_.identify(display, seconds);
            reply(list());
        });
    }
    if (op == "unmirror") {
        const std::optional<bool> on =
            message.value("on", Message()).is_boolean() ? std::optional(message["on"].get<bool>()) : std::nullopt;
        return post_([this, reply, on] {
            if (on)
                return setUnmirror(*on);
            reply(list());
        });
    }
    reply({{"type", "error"}, {"message", "unknown outputs op " + op}});
}

void OutputService::setUnmirror(bool on)
{
    known_.setUnmirror(on);
    log(std::string("ending display mirroring between screens turned ") + (on ? "on" : "off"));
    keep();
}

Message OutputService::list() const
{
    Message displays = Message::array();
    for (const DisplayInfo& d : displays_.attached()) {
        Message display{{"id", d.id},
                        {"name", displayName(d.id)},
                        {"width", d.width},
                        {"height", d.height},
                        {"refreshHz", d.refreshHz}};
        if (!d.mirrorOf.empty())
            display["mirrorOf"] = d.mirrorOf;
        displays.push_back(display);
    }
    Message outputs = Message::array();
    for (DisplayOutput* o : outputs_.all()) {
        Message output{
            {"display", o->display()}, {"screen", o->screen()}, {"attached", o->attached()}, {"main", o->main()}};
        if (const auto target = outputs_.target(o->display()))
            output["target"] = *target;
        outputs.push_back(output);
    }
    return {{"type", "outputs"},
            {"op", "list"},
            {"displays", displays},
            {"outputs", outputs},
            {"guides", outputs_.guides()},
            {"unmirror", known_.unmirror()},
            {"roles", screens_.roles()}};
}

} // namespace azl
