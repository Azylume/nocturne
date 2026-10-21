#include "protocol/remote_files.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <fstream>

namespace {

// A machine's folders: the shows and media a client on another machine may use, and a secret outside them.
struct Folders {
    Folders()
    {
        for (const char* d : {"shows", "media", "record", "elsewhere"})
            fs::create_directories(root.path / d);
        std::ofstream(root.path / "elsewhere/secret.txt") << "secret\n";
        fs::create_directory_symlink(root.path / "elsewhere", root.path / "shows/escape");
        fs::create_symlink(root.path / "elsewhere/secret.txt", root.path / "media/secret.mp4");
    }

    fs::path operator/(const std::string& name) const { return root.path / name; }
    std::vector<fs::path> roots() const { return {root.path / "shows", root.path / "media", root.path / "record"}; }

    TempDir root;
};

std::string refusal(const std::string& path, const std::vector<fs::path>& roots)
{
    return azl::pathRefusal(path, roots).value_or("");
}

bool refused(const Message& reply, const std::string& why)
{
    INFO(reply.dump());
    return reply["type"] == "error" && reply["message"].get<std::string>().find(why) != std::string::npos;
}

} // namespace

TEST_CASE("a client on another machine may name paths inside the allowed folders only: no \"..\", no symbolic link "
          "out, no folder that only starts like one")
{
    Folders f;
    const std::vector<fs::path> roots = f.roots();
    CHECK(refusal((f / "shows/tonight.json").string(), roots).empty());
    CHECK(refusal((f / "shows").string(), roots).empty());
    CHECK(refusal((f / "shows/new/deeper/show.json").string(), roots).empty());
    CHECK(refusal((f / "media/clip.mp4").string(), roots).empty());

    CHECK(refusal("shows/tonight.json", roots).find("not an absolute path") != std::string::npos);
    CHECK(refusal((f / "shows/../elsewhere/secret.txt").string(), roots).find("\"..\"") != std::string::npos);
    CHECK(refusal((f / "shows/missing/../../elsewhere/x").string(), roots).find("\"..\"") != std::string::npos);
    CHECK(refusal((f / "elsewhere/secret.txt").string(), roots).find("outside") != std::string::npos);
    CHECK(refusal((f / "shows/escape/secret.txt").string(), roots).find("outside") != std::string::npos);
    CHECK(refusal((f / "shows/escape/new.json").string(), roots).find("outside") != std::string::npos);
    CHECK(refusal((f / "media/secret.mp4").string(), roots).find("outside") != std::string::npos);
    CHECK(refusal((f.root.path / "shows-old/show.json").string(), roots).find("outside") != std::string::npos);
    CHECK_FALSE(refusal("/etc/passwd", roots).empty());
    CHECK_FALSE(refusal((f / "shows/tonight.json").string(), {}).empty());

    const char* home = std::getenv("HOME");
    REQUIRE(home);
    CHECK(refusal("~/Nocturne/shows/a.json", {fs::path(home) / "Nocturne/shows"}).empty());
    CHECK_FALSE(refusal("~/Documents/a.json", {fs::path(home) / "Nocturne/shows"}).empty());
}

TEST_CASE("the path fields of composition, library, record and preset messages are checked; other messages pass")
{
    Folders f;
    const std::vector<fs::path> roots = f.roots();
    const std::string out = (f / "elsewhere/secret.txt").string();
    const std::string in = (f / "shows/a.json").string();
    const std::vector<Message> outside{
        {{"type", "composition"}, {"op", "save"}, {"path", out}},
        {{"type", "composition"}, {"op", "save"}, {"path", in}, {"mediaRoot", (f / "elsewhere").string()}},
        {{"type", "composition"}, {"op", "load"}, {"path", out}},
        {{"type", "composition"}, {"op", "shows"}, {"folder", "/"}},
        {{"type", "library"}, {"op", "add"}, {"path", out}},
        {{"type", "library"}, {"op", "relink"}, {"hash", "abc"}, {"path", out}},
        {{"type", "record"}, {"op", "set"}, {"path", "/tmp"}},
        {{"type", "record"}, {"op", "folders"}, {"path", "/"}},
        {{"type", "preset"}, {"op", "import"}, {"roots", Message::array({(f / "media").string(), "/"})}},
        {{"type", "library"}, {"op", "add"}, {"path", 7}},
    };
    for (const Message& m : outside) {
        CAPTURE(m.dump());
        CHECK(azl::remotePathRefusal(m, roots));
    }
    const std::vector<Message> fine{
        {{"type", "composition"}, {"op", "save"}, {"path", in}, {"mediaRoot", (f / "media").string()}},
        {{"type", "composition"}, {"op", "shows"}},
        {{"type", "record"}, {"op", "folders"}},
        {{"type", "record"}, {"op", "set"}, {"path", (f / "record/night").string()}},
        {{"type", "preset"}, {"op", "import"}},
        {{"type", "library"}, {"op", "list"}, {"path", out}},
        {{"type", "machine"}, {"op", "status"}},
    };
    for (const Message& m : fine) {
        CAPTURE(m.dump());
        CHECK_FALSE(azl::remotePathRefusal(m, roots));
    }
}

TEST_CASE("over the protocol a remote client saves and loads only inside the allowed folders, while a loopback client "
          "keeps full access")
{
    Folders f;
    azl::EngineHarness harness({.width = 160, .height = 90}, 60.0, f / "library");
    Client remote(harness.document(), false);
    Client local(harness.document());
    const fs::path outside = f / "elsewhere/show.json";
    const fs::path inside = f / "shows/show.json";

    remote.send({{"type", "composition"}, {"op", "save"}, {"path", inside.string()}});
    CHECK(refused(remote.next(), "outside"));
    CHECK_FALSE(fs::exists(inside));

    harness.document().allowRemoteFiles([&] { return f.roots(); });
    remote.send({{"type", "composition"}, {"op", "save"}, {"path", outside.string()}});
    CHECK(refused(remote.next(), "outside"));
    CHECK_FALSE(fs::exists(outside));
    remote.send({{"type", "composition"}, {"op", "save"}, {"path", (f / "shows/escape/show.json").string()}});
    CHECK(refused(remote.next(), "outside"));
    CHECK_FALSE(fs::exists(f / "elsewhere/show.json"));

    remote.send({{"type", "composition"}, {"op", "save"}, {"path", inside.string()}});
    Message reply = remote.next();
    CHECK_MESSAGE(reply["type"] == "composition", reply.dump());
    CHECK(fs::exists(inside));

    remote.send({{"type", "library"}, {"op", "add"}, {"path", (f / "elsewhere/secret.txt").string()}});
    CHECK(refused(remote.next(), "outside"));
    remote.send({{"type", "library"}, {"op", "add"}, {"path", (f / "media/secret.mp4").string()}});
    CHECK(refused(remote.next(), "outside"));
    remote.send({{"type", "record"}, {"op", "folders"}, {"path", "/"}});
    CHECK(refused(remote.next(), "outside"));

    local.send({{"type", "composition"}, {"op", "save"}, {"path", outside.string()}});
    for (reply = local.next(); reply["type"] == "change"; reply = local.next())
        ;
    CHECK_MESSAGE(reply["type"] == "composition", reply.dump());
    CHECK(fs::exists(outside));
}
