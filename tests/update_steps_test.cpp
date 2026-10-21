#include "engine/bundle_update.hpp"
#include "test_client.hpp"
#include "test_paths.hpp"

#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// tools/update-bundle.sh, the steps deploy-mini.sh and Update share, against a fake venue: a folder with a running
// Nocturne.app whose supervisor, engine and ctl are scripts, and a fake autostart agent that starts and stops it.
namespace {

constexpr const char* kOldBuild = "20261017000000-aaaa";
constexpr const char* kNewBuild = "20261018000000-bbbb";

// healthy: answers with its build and renders; crashing: answers for a second, then exits, again and again;
// wrongBuild: answers with another build id. The unhealthy ones also overwrite the autosaved state, as a new
// composition format would.
enum class Engine { Healthy, Crashing, WrongBuild };

std::string updateSteps(const fs::path& app)
{
    return "/bin/bash '" + (sourceDir() / "tools/update-bundle.sh").string() + "' '" + app.string() + "'";
}

constexpr const char* kSupervisor = R"sh(#!/bin/bash
dir=$(cd "$(dirname "$0")" && pwd)
starts=0 pid=
trap '[[ -n $pid ]] && kill $pid; wait; exit 0' TERM
while :; do
    starts=$((starts + 1))
    "$dir/nocturne" & pid=$!
    printf '{"mode": "normal", "starts": %d, "engine": %d}\n' $starts $pid > "$STATE/status.json"
    wait $pid || true
    sleep 0.2
done
)sh";

constexpr const char* kCtl = R"sh(#!/bin/bash
pid=$(plutil -extract engine raw -o - "$STATE/status.json" 2> /dev/null) && kill -0 "$pid" 2> /dev/null &&
    [[ -f $STATE/engine-$pid.json ]] || { echo "nocturne-ctl: no snapshot" >&2; exit 1; }
cat "$STATE/engine-$pid.json"
)sh";

constexpr const char* kAgent = R"sh(#!/bin/bash
pidfile=$VENUE/agent.pid
case $1 in
    running) [[ -f $pidfile ]] && kill -0 "$(cat "$pidfile")" 2> /dev/null ;;
    stop)
        pid=$(cat "$pidfile")
        kill "$pid"
        while kill -0 "$pid" 2> /dev/null; do sleep 0.05; done
        rm -f "$pidfile"
        ;;
    start) "$VENUE/Nocturne.app/Contents/MacOS/nocturne-supervisor" > /dev/null 2>&1 & echo $! > "$pidfile" ;;
esac
)sh";

void writeScript(const fs::path& path, const std::string& text)
{
    std::ofstream(path) << text;
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::add);
}

void makeBundle(const fs::path& app, const std::string& build, Engine engine)
{
    const fs::path bin = app / "Contents/MacOS";
    fs::create_directories(bin);
    const std::string version = engine == Engine::WrongBuild ? "20200101000000-ffff" : build;
    std::ostringstream script;
    script << "#!/bin/bash\n# nocturne-build " << build << "\ntrap 'exit 0' TERM\n";
    if (engine != Engine::Healthy)
        script << "echo spoiled > \"$STATE/composition.json\"\n";
    script << "printf '{\"mode\":\"normal\",\"op\":\"status\",\"render\":{\"fps\":60},\"type\":\"machine\",\"version\":\""
           << version << "\"}\\n' > \"$STATE/engine-$$.json\"\n";
    script << "while :; do " << (engine == Engine::Crashing ? "(( SECONDS < 1 )) || exit 1; " : "") << "sleep 0.1; done\n";
    writeScript(bin / "nocturne", script.str());
    writeScript(bin / "nocturne-supervisor", kSupervisor);
    writeScript(bin / "nocturne-ctl", kCtl);
}

std::string read(const fs::path& path)
{
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

struct Venue {
    Venue()
    {
        for (const char* d : {"venue", "state", "logs", "laptop"})
            fs::create_directories(root.path / d);
        writeScript(root.path / "agent.sh", kAgent);
        makeBundle(venue() / "Nocturne.app", kOldBuild, Engine::Healthy);
        std::ofstream(state() / "composition.json") << "{\"show\": \"Reference Show\"}\n";
        REQUIRE(agent("start") == 0);
    }
    ~Venue() { agent("stop"); }

    fs::path venue() const { return root.path / "venue"; }
    fs::path state() const { return root.path / "state"; }

    std::string environment() const
    {
        return "VENUE='" + venue().string() + "' STATE='" + state().string() + "' LOGS='" + (root.path / "logs").string() +
               "' AGENT='" + (root.path / "agent.sh").string() + "' VERIFY=true HEALTH_TIMEOUT=15 HEALTH_STABLE=2 ";
    }

    int agent(const std::string& op) const
    {
        return std::system((environment() + "'" + (root.path / "agent.sh").string() + "' " + op).c_str());
    }

    // Runs the update steps on a new bundle with an engine like `engine`; the exit status.
    int update(Engine engine)
    {
        const fs::path app = root.path / "laptop/Nocturne.app";
        makeBundle(app, kNewBuild, engine);
        const std::string command = environment() + updateSteps(app) + " 2>&1";
        FILE* pipe = popen(command.c_str(), "r");
        REQUIRE(pipe);
        char buffer[512];
        while (fgets(buffer, sizeof buffer, pipe))
            output += buffer;
        const int status = pclose(pipe);
        return WEXITSTATUS(status);
    }

    Message result() const { return Message::parse(read(state() / "update.json")); }
    std::string running() const
    {
        const std::string engine = read(venue() / "Nocturne.app/Contents/MacOS/nocturne");
        const std::string marker = "nocturne-build ";
        return engine.substr(engine.find(marker) + marker.size(), std::string(kOldBuild).size());
    }
    bool backedUp() const
    {
        for (const auto& entry : fs::directory_iterator(state() / "backups"))
            if (entry.path().filename().string().ends_with("-update.json") &&
                read(entry.path()) == "{\"show\": \"Reference Show\"}\n")
                return true;
        return false;
    }

    TempDir root;
    std::string output;
};

} // namespace

TEST_CASE("update steps: a healthy bundle replaces the running one under the supervisor and the previous is deleted")
{
    Venue venue;
    CHECK(venue.update(Engine::Healthy) == 0);
    INFO(venue.output);
    CHECK(venue.running() == kNewBuild);
    CHECK(venue.agent("running") == 0);
    CHECK_FALSE(fs::exists(venue.venue() / "Nocturne.app.previous"));
    CHECK(venue.result()["result"] == "updated");
    CHECK(venue.result()["build"] == kNewBuild);
    CHECK(venue.result()["previous"] == kOldBuild);
    CHECK(venue.backedUp());
    CHECK(read(venue.state() / "composition.json") == "{\"show\": \"Reference Show\"}\n");
    CHECK(read(venue.root.path / "logs/update.log").find("updated") != std::string::npos);
}

TEST_CASE("update steps: a bundle whose engine keeps crashing under the supervisor is rolled back with its state")
{
    Venue venue;
    CHECK(venue.update(Engine::Crashing) == 1);
    INFO(venue.output);
    CHECK(venue.running() == kOldBuild);
    CHECK(venue.agent("running") == 0);
    CHECK_FALSE(fs::exists(venue.venue() / "Nocturne.app.previous"));
    CHECK(venue.result()["result"] == "rolled back");
    const std::string reason = venue.result()["reason"];
    CHECK((reason.find("restarted by the supervisor") != std::string::npos ||
           reason.find("does not answer") != std::string::npos));
    CHECK(read(venue.state() / "composition.json") == "{\"show\": \"Reference Show\"}\n");
    CHECK(venue.backedUp());
}

TEST_CASE("update steps: a bundle whose engine reports another build id is rolled back, and the old one plays again")
{
    Venue venue;
    CHECK(venue.update(Engine::WrongBuild) == 1);
    INFO(venue.output);
    CHECK(venue.running() == kOldBuild);
    CHECK(venue.result()["result"] == "rolled back");
    CHECK(venue.result()["reason"].get<std::string>().find("reports build 20200101000000-ffff, not " +
                                                           std::string(kNewBuild)) != std::string::npos);
    CHECK(read(venue.state() / "composition.json") == "{\"show\": \"Reference Show\"}\n");
    const std::string ctl = venue.environment() + "'" + (venue.venue() / "Nocturne.app/Contents/MacOS/nocturne-ctl").string() +
                            "' | grep -q '\"version\":\"" + kOldBuild + "\"'";
    CHECK(std::system(ctl.c_str()) == 0);
}

TEST_CASE("update steps: without a running agent the bundles are only swapped; a bundle without a build id is refused")
{
    Venue venue;
    REQUIRE(venue.agent("stop") == 0);
    CHECK(venue.update(Engine::Healthy) == 0);
    CHECK(venue.running() == kNewBuild);
    CHECK(venue.result()["result"] == "installed");
    CHECK(fs::exists(venue.venue() / "Nocturne.app.previous"));
    CHECK(venue.agent("running") != 0);

    fs::create_directories(venue.root.path / "laptop/Empty.app");
    const std::string command =
        venue.environment() + updateSteps(venue.root.path / "laptop/Empty.app") + " > /dev/null 2>&1";
    const int status = std::system(command.c_str());
    CHECK(WEXITSTATUS(status) == 1);
    CHECK(venue.result()["result"] == "failed");
    CHECK(venue.running() == kNewBuild);
    REQUIRE(venue.agent("start") == 0);
}

TEST_CASE("the engine's installer runs the update script in a session of its own, with the venue and state it names")
{
    TempDir dir;
    const fs::path script = dir.path / "update-bundle.sh";
    writeScript(script, "#!/bin/bash\necho \"$1|$VENUE|$STATE|$$|$(ps -o pgid= -p $$ | tr -d ' ')\" > \"$STATE/ran\"\n");
    const azl::InstallBundle install =
        azl::bundleInstaller(script, {{"VENUE", "/somewhere/Nocturne"}, {"STATE", dir.path.string()}});
    CHECK_FALSE(install(dir.path / "Nocturne.app"));
    std::string ran;
    for (int i = 0; i < 250 && ran.empty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ran = read(dir.path / "ran");
    }
    REQUIRE_FALSE(ran.empty());
    std::istringstream fields(ran);
    std::string app, venue, state, pid, group;
    std::getline(fields, app, '|');
    std::getline(fields, venue, '|');
    std::getline(fields, state, '|');
    std::getline(fields, pid, '|');
    std::getline(fields, group);
    CHECK(app == (dir.path / "Nocturne.app").string());
    CHECK(venue == "/somewhere/Nocturne");
    CHECK(state == dir.path.string());
    CHECK(pid == group);
    CHECK(group != std::to_string(getpgrp()));
    CHECK(azl::bundleInstaller(dir.path / "missing.sh", {})(dir.path));
}

// tests/signing holds fake bundles signed by throwaway identities (make_signed_bundles.sh): running.app and same.app by
// one, other.app by another certificate with the same name and identifier.
TEST_CASE("Update installs only a bundle signed by the running bundle's identity, and nothing over an ad hoc one")
{
    const fs::path signing = sourceDir() / "tests/signing";
    const fs::path running = signing / "running.app";
    CHECK_FALSE(azl::signerMissing(running));
    CHECK_FALSE(azl::signerRefusal(running, signing / "same.app"));

    const std::optional<std::string> other = azl::signerRefusal(running, signing / "other.app");
    REQUIRE(other);
    CHECK(other->find("not signed by the identity") != std::string::npos);
    CHECK(azl::signerRefusal(running, signing / "adhoc.app"));
    CHECK(azl::signerRefusal(running, signing / "unsigned.app"));

    TempDir dir;
    fs::copy(signing / "same.app", dir.path / "Nocturne.app", fs::copy_options::recursive);
    std::ofstream(dir.path / "Nocturne.app/Contents/MacOS/nocturne", std::ios::app) << "echo tampered\n";
    CHECK(azl::signerRefusal(running, dir.path / "Nocturne.app"));

    for (const char* unsignedRunning : {"adhoc.app", "unsigned.app"}) {
        const std::optional<std::string> missing = azl::signerMissing(signing / unsignedRunning);
        REQUIRE(missing);
        CHECK(azl::signerRefusal(signing / unsignedRunning, signing / "same.app") == missing);
        CHECK(azl::signerRefusal(signing / unsignedRunning, signing / unsignedRunning) == missing);
    }
    CHECK(azl::signerMissing(signing / "adhoc.app")->find("ad hoc") != std::string::npos);
}
