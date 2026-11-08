#include "engine/machine_control.hpp"
#include "net/websocket.hpp"
#include "platform/process.hpp"
#include "scoped_env.hpp"
#include "supervisor/log_tail.hpp"
#include "temp_folder.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <fstream>
#include <sstream>

namespace {

constexpr azl::EngineConfig kSize{.width = 160, .height = 90};

void write(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string read(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

Message ask(Client& client, Message request)
{
    request["type"] = "machine";
    client.send(request);
    for (;;)
        if (Message m = client.next(); (m["type"] == "machine" && m["op"] == request["op"]) || m["type"] == "error")
            return m;
}

bool refusedWith(const Message& reply, const std::string& text)
{
    INFO(reply.dump());
    return reply["type"] == "error" && reply["message"].get<std::string>().find(text) != std::string::npos;
}

// A home folder with ~/Nocturne/shows and ~/Nocturne/media, so settings paths are confined to it.
struct Home {
    Home() : env("HOME", root.path.string().c_str())
    {
        fs::create_directories(shows());
        fs::create_directories(media());
    }
    fs::path shows() const { return root.path / "Nocturne" / "shows"; }
    fs::path media() const { return root.path / "Nocturne" / "media"; }
    TempFolder root;
    ScopedEnv env;
};

Message supervisorOf(Client& client)
{
    client.send({{"type", "machine"}, {"op", "status"}});
    for (;;)
        if (Message m = client.next(); m["type"] == "machine" && m["op"] == "status")
            return m["supervisor"];
}

Message settings(Client& client, Message fields = Message::object())
{
    fields["op"] = "settings";
    return ask(client, fields);
}

} // namespace

TEST_CASE("supervisor status: the machine status carries the Supervisor's status.json and the autostart state")
{
    TempDir root;
    azl::EngineHarness harness(kSize, 60.0, root.path);
    harness.setAutostart(azl::AutostartState::On);
    Client client(harness.document());

    write(root.path / "status.json", Message{{"mode", "normal"},
                                             {"reason", ""},
                                             {"error", ""},
                                             {"engine", 4242},
                                             {"starts", 3},
                                             {"startedAt", 1790000000.5},
                                             {"crashes", Message::array({"2026-10-16 hung"})},
                                             {"supervisor", azl::currentProcessId()}}
                                         .dump());
    const Message alive = supervisorOf(client);
    CHECK(alive["running"] == true);
    CHECK(alive["mode"] == "normal");
    CHECK(alive["reason"].is_null());
    CHECK(alive["enginePid"] == 4242);
    CHECK(alive["startedAt"] == 1790000000.5);
    CHECK(alive["starts"] == 3);
    CHECK(alive["crashes"] == Message::array({"2026-10-16 hung"}));
    CHECK(alive["autostart"] == "on");

    write(root.path / "status.json",
          Message{{"mode", "safe"}, {"reason", "3 crashes"}, {"supervisor", 2147483000}}.dump());
    const Message dead = supervisorOf(client);
    CHECK(dead["running"] == false);
    CHECK(dead["mode"] == "safe");
    CHECK(dead["reason"] == "3 crashes");

    write(root.path / "status.json", "{ half written");
    const Message broken = supervisorOf(client);
    CHECK(broken["running"] == false);
    CHECK(broken["mode"].is_null());

    write(root.path / "status.json", R"({"supervisor": "me", "engine": [1], "starts": {}})");
    CHECK(supervisorOf(client)["running"] == false);

    fs::remove(root.path / "status.json");
    CHECK(supervisorOf(client)["running"] == false);
}

TEST_CASE("supervisor status: an engine without a supervisor reports none, and its control ops say so")
{
    azl::MachineControl control({}, [](const std::string&) { return std::nullopt; });
    CHECK(control.supervisor().is_null());
    for (const char* op : {"autostart", "settings", "logs"}) {
        Message reply;
        CHECK(control.handle({{"type", "machine"}, {"op", op}}, [&](const Message& m) { reply = m; }));
        CHECK(refusedWith(reply, "needs the supervisor"));
    }
    CHECK_FALSE(control.handle({{"type", "machine"}, {"op", "restart"}}, [](const Message&) {}));
}

TEST_CASE("autostart: on and off go through the platform and show in the next status; a failure changes nothing")
{
    TempDir root;
    azl::EngineHarness harness(kSize, 60.0, root.path);
    harness.setAutostart(azl::AutostartState::On);
    Client client(harness.document());
    CHECK(supervisorOf(client)["autostart"] == "on");

    CHECK(ask(client, {{"op", "autostart"}, {"on", false}}) ==
          Message{{"type", "machine"}, {"op", "autostart"}, {"on", false}});
    CHECK(supervisorOf(client)["autostart"] == "off");
    CHECK(ask(client, {{"op", "autostart"}, {"on", true}})["on"] == true);
    CHECK(supervisorOf(client)["autostart"] == "on");

    CHECK(refusedWith(ask(client, {{"op", "autostart"}}), "\"on\": true or false"));
    CHECK(refusedWith(ask(client, {{"op", "autostart"}, {"on", "yes"}}), "\"on\": true or false"));

    harness.setAutostart(azl::AutostartState::On, "launchctl disable failed: no right");
    CHECK(refusedWith(ask(client, {{"op", "autostart"}, {"on", false}}), "no right"));
    CHECK(supervisorOf(client)["autostart"] == "on");
}

TEST_CASE("autostart and settings writes are refused under the Show lock and in a Gig window, reads are not")
{
    TempDir root;
    Home home;
    azl::EngineHarness harness(kSize, 60.0, root.path);
    harness.setAutostart(azl::AutostartState::On);
    const azl::UtcOffset utc = [](int64_t) { return 0; };
    const int64_t day = *azl::dateDays("2026-10-16");
    harness.setWallClock(double(azl::unixAt(day, 21 * 60, utc)), utc);
    Client client(harness.document());
    const Message save{{"composition", (home.shows() / "a.json").string()}};

    CHECK(ask(client, {{"op", "lock"}, {"on", true}})["on"] == true);
    CHECK(refusedWith(ask(client, {{"op", "autostart"}, {"on", false}}), "Show lock is on: Autostart is refused"));
    CHECK(refusedWith(settings(client, save), "Show lock is on: Settings is refused"));
    CHECK_FALSE(fs::exists(root.path / "machine-settings.json"));
    CHECK(supervisorOf(client)["autostart"] == "on");
    CHECK(settings(client)["op"] == "settings");
    CHECK(ask(client, {{"op", "logs"}, {"file", "engine"}})["op"] == "logs");
    CHECK(ask(client, {{"op", "lock"}, {"on", false}})["on"] == false);

    apply(client, azl::SetGigWindow{"2026-10-16", azl::GigWindow{"2026-10-16", 20 * 60, 3 * 60}});
    CHECK(refusedWith(ask(client, {{"op", "autostart"}, {"on", false}}),
                      "Gig window 2026-10-16 20:00-03:00 is on: Autostart"));
    CHECK(refusedWith(settings(client, save), "Gig window 2026-10-16 20:00-03:00 is on: Settings"));
    CHECK(settings(client)["op"] == "settings");

    harness.setWallClock(double(azl::unixAt(day + 1, 3 * 60, utc)), utc);
    CHECK(settings(client, save)["saved"]["composition"] == save["composition"]);
    CHECK(ask(client, {{"op", "autostart"}, {"on", false}})["on"] == false);
}

TEST_CASE("machine settings: saved fields are read back, applied only at the next start, and an empty one clears it")
{
    TempDir root;
    Home home;
    azl::EngineHarness harness(kSize, 60.0, root.path);
    Client client(harness.document());

    const Message none = settings(client);
    CHECK(none["saved"] == Message{{"composition", ""}, {"mediaRoot", ""}, {"audioInput", ""}});
    CHECK(none["sources"]["composition"] == "default");
    CHECK_FALSE(none.contains("error"));

    const std::string show = (home.shows() / "Friday.json").string();
    const Message saved =
        settings(client, {{"composition", show}, {"mediaRoot", home.media().string()}, {"audioInput", "fake-mic"}});
    CHECK(saved["saved"] ==
          Message{{"composition", show}, {"mediaRoot", home.media().string()}, {"audioInput", "fake-mic"}});
    CHECK(saved["running"]["composition"] == "");
    CHECK(Message::parse(read(root.path / "machine-settings.json"))["audioInput"] == "fake-mic");

    const Message partial = settings(client, {{"audioInput", ""}});
    CHECK(partial["saved"]["composition"] == show);
    CHECK(partial["saved"]["audioInput"] == "");

    write(root.path / "status.json", Message{{"applied", Message::array({"composition"})}}.dump());
    CHECK(settings(client)["sources"]["composition"] == "saved");
    CHECK(settings(client)["sources"]["mediaRoot"] == "default");
}

TEST_CASE("machine settings: a file that cannot be used reads as empty with its error, and a write replaces it")
{
    TempDir root;
    Home home;
    azl::EngineHarness harness(kSize, 60.0, root.path);
    Client client(harness.document());

    write(root.path / "machine-settings.json", "[1, 2");
    const Message broken = settings(client);
    CHECK(broken["saved"]["composition"] == "");
    CHECK(broken["error"].get<std::string>().find("machine-settings.json") != std::string::npos);

    write(root.path / "machine-settings.json", Message{{"mediaRoot", "/etc"}}.dump());
    CHECK(settings(client)["error"].get<std::string>().find("mediaRoot") != std::string::npos);

    CHECK_FALSE(settings(client, {{"audioInput", "fake-mic"}}).contains("error"));
    CHECK(Message::parse(read(root.path / "machine-settings.json"))["mediaRoot"] == "");
}

TEST_CASE("machine settings: paths outside ~/Nocturne/shows and media, relative, traversing, linked out or very long "
          "are refused")
{
    TempDir root;
    Home home;
    azl::EngineHarness harness(kSize, 60.0, root.path);
    Client client(harness.document());
    const fs::path outside = home.root.path / "outside";
    fs::create_directories(outside);

    const auto refused = [&](const char* field, const std::string& path, const std::string& why) {
        INFO(field << " " << path.substr(0, 80));
        CHECK(refusedWith(settings(client, {{field, path}}), why));
        CHECK_FALSE(fs::exists(root.path / "machine-settings.json"));
    };
    refused("composition", "/etc/passwd", "outside");
    refused("composition", (home.media() / "a.json").string(), "outside");
    refused("mediaRoot", home.shows().string(), "outside");
    refused("mediaRoot", outside.string(), "outside");
    refused("composition", "shows/a.json", "not an absolute path");
    refused("composition", "./a.json", "not an absolute path");
    refused("composition", (home.shows() / ".." / ".." / "outside" / "a.json").string(), "\"..\"");
    refused("mediaRoot", home.media().string() + "/../x", "\"..\"");
    refused("composition", (home.shows() / std::string(2000, 'a')).string(), "not a usable path");
    refused("composition", home.shows().string() + "/a\nb.json", "not a usable path");
    refused("composition", home.shows().string() + std::string("/a\0b.json", 9), "not a usable path");
    refused("audioInput", std::string(300, 'x'), "not a usable device name");
    refused("audioInput", "mic\n--composition", "not a usable device name");
#ifndef _WIN32
    fs::create_symlink(outside, home.shows() / "escape");
    fs::create_symlink(outside, home.media() / "escape");
    refused("composition", (home.shows() / "escape" / "a.json").string(), "outside");
    refused("mediaRoot", (home.media() / "escape").string(), "outside");
#endif
    CHECK(refusedWith(settings(client, {{"composition", 5}}), "must be text"));
    CHECK(refusedWith(settings(client, {{"audioInput", "no such input"}}), "lists no input"));
    CHECK(settings(client, {{"composition", (home.shows() / "sub" / "a.json").string()}})["saved"]["composition"] ==
          (home.shows() / "sub" / "a.json").string());
}

TEST_CASE("machine logs: the newest lines of the engine and supervisor logs, capped, and no other file")
{
    TempDir root;
    azl::EngineHarness harness(kSize, 60.0, root.path);
    Client client(harness.document());
    const auto logs = [&](Message request) {
        request["op"] = "logs";
        return ask(client, request);
    };

    CHECK(logs({{"file", "engine"}}) ==
          Message{{"type", "machine"}, {"op", "logs"}, {"file", "engine"}, {"text", ""}, {"truncated", false}});

    write(root.path / "logs" / "engine.log", "one\ntwo\nthree\n");
    write(root.path / "logs" / "supervisor.log", "supervisor line\n");
    write(root.path / "logs" / "audio.jsonl", "secret\n");
    write(root.path / "token", "TOKEN");
    CHECK(logs({{"file", "engine"}})["text"] == "one\ntwo\nthree\n");
    CHECK(logs({{"file", "engine"}, {"lines", 2}}) ==
          Message{
              {"type", "machine"}, {"op", "logs"}, {"file", "engine"}, {"text", "two\nthree\n"}, {"truncated", true}});
    CHECK(logs({{"file", "supervisor"}})["text"] == "supervisor line\n");

    for (const char* file :
         {"audio", "audio.jsonl", "../token", "../../etc/passwd", "engine.log", "desk", "", "Engine"})
        CHECK(refusedWith(logs({{"file", file}}), "\"engine\" or \"supervisor\""));
    CHECK(refusedWith(logs({}), "\"engine\" or \"supervisor\""));
    CHECK(refusedWith(logs({{"file", 3}}), "\"engine\" or \"supervisor\""));
    for (const Message& lines : {Message(0), Message(201), Message(-1), Message(1.5), Message("5")})
        CHECK(refusedWith(logs({{"file", "engine"}, {"lines", lines}}), "from 1 to 200"));
    CHECK(logs({{"file", "engine"}, {"lines", 200}})["type"] == "machine");
}

TEST_CASE("log tail: at most 200 lines and 64 KiB, cut at a line start, from the file's end")
{
    TempFolder root;
    const fs::path file = root.path / "big.log";

    std::string many;
    for (int i = 0; i < 1000; ++i)
        many += "line " + std::to_string(i) + "\n";
    write(file, many);
    const azl::LogTail lines = azl::tailLog(file, 100000);
    CHECK(lines.truncated);
    CHECK(lines.text.size() == read(file).size() - read(file).find("line 800\n"));
    CHECK(lines.text.rfind("line 800\n", 0) == 0);
    CHECK(lines.text.ends_with("line 999\n"));

    std::string wide;
    for (int i = 0; i < 2000; ++i)
        wide += std::to_string(i) + std::string(98, '.') + "\n";
    write(file, wide);
    const azl::LogTail bytes = azl::tailLog(file, azl::kMaxLogLines);
    CHECK(bytes.text.size() <= azl::kMaxLogBytes);
    CHECK(std::count(bytes.text.begin(), bytes.text.end(), '\n') == 200);
    CHECK(bytes.text.ends_with(std::to_string(1999) + std::string(98, '.') + "\n"));

    write(file, std::string(200000, 'x'));
    const azl::LogTail noNewline = azl::tailLog(file, 5);
    CHECK(noNewline.text.empty());
    CHECK(noNewline.truncated);

    write(file, "a\nb\nlast without newline");
    CHECK(azl::tailLog(file, 2).text == "b\nlast without newline");
    CHECK_FALSE(azl::tailLog(file, 3).truncated);
    CHECK(azl::tailLog(root.path / "missing.log", 5).text.empty());
}

TEST_CASE("nocturne-ctl machine autostart, settings and logs reach the machine control and print its answers")
{
    TempDir root;
    Home home;
    azl::EngineHarness harness(kSize, 60.0, root.path);
    harness.setAutostart(azl::AutostartState::On);
    azl::WebSocketServer server(harness.document(), 0);
    const auto ctl = [&](const std::string& args) {
        const CtlOutput run = runNocturneCtl(server.port(), args);
        return std::pair{run.code, run.output};
    };
    const std::string show = (home.shows() / "Friday.json").string();

    auto [code, out] = ctl("machine autostart off");
    CHECK(code == 0);
    CHECK(out.find("\"on\":false") != std::string::npos);
    Client client(harness.document());
    CHECK(supervisorOf(client)["autostart"] == "off");

    CHECK(ctl("machine settings").first == 0);
    std::tie(code, out) = ctl("machine settings composition " + show + " audioinput fake-mic");
    CHECK(code == 0);
    CHECK(Message::parse(read(root.path / "machine-settings.json"))["composition"] == show);
    std::tie(code, out) = ctl("machine settings mediaroot /etc");
    CHECK(code == 1);
    CHECK(out.find("outside") != std::string::npos);
    CHECK(ctl("machine settings bogus x").first == 2);

    write(root.path / "logs" / "engine.log", "one\ntwo\nthree\n");
    std::tie(code, out) = ctl("machine logs engine 2");
    CHECK(code == 0);
    CHECK(out.find("two\nthree\n") != std::string::npos);
    CHECK(out.find("one") == std::string::npos);
    CHECK(ctl("machine logs audio").first == 1);

    CHECK(ctl("machine lock on").first == 0);
    std::tie(code, out) = ctl("machine autostart on");
    CHECK(code == 1);
    CHECK(out.find("Show lock is on") != std::string::npos);
}
