#include "net/websocket.hpp"
#include "ui/app/main_component.hpp"
#include "ui/app/midi.hpp"
#include "ui/client/engine_launcher.hpp"

#include <algorithm>
#include <cctype>

namespace {

// Runs the engine watchdog on the message thread. Each tick it restarts an engine of ours that died (the launcher
// bounds that to twice a minute, then gives up); if it gives up, the failure shows in the UI. When the UI reused
// another instance's engine and that engine has gone (the other instance quit), it starts one of its own after a few
// seconds disconnected.
class EngineKeepalive : public juce::Timer {
public:
    EngineKeepalive(azl::ui::EngineLauncher& launcher, azl::ui::Session& session) : launcher_(launcher), session_(session)
    {
        startTimer(1000);
    }

    void timerCallback() override
    {
        launcher_.poll();
        if (!launcher_.failure().empty()) {
            session_.setEngineError(launcher_.failure());
            return;
        }
        if (session_.client().connected() || launcher_.startedByUs()) {
            down_ = 0;
            return;
        }
        if (++down_ >= kDownToAdopt)
            launcher_.ensureRunning();
    }

private:
    static constexpr int kDownToAdopt = 10; // About 10 s disconnected before taking over a reused engine that is gone.
    azl::ui::EngineLauncher& launcher_;
    azl::ui::Session& session_;
    int down_ = 0;
};

// nocturne-ui [--host H] [--port N] [--jpeg-previews] [--remote-previews | --local-previews] [--hidden]
// [--snapshot FILE [--snapshots N] [--snapshot-after S]] [--map [--map-screen N] [--map-point K] [--map-input]]:
// connects to the engine on H, a name or an address (default the last H given, else this machine), at port N (default
// 7400); hidden and snapshot runs neither use nor remember the last H, and list no venues. --jpeg-previews takes
// previews as a remote UI does. Previews of an engine on another machine are rendered on this one; --remote-previews leaves them to
// the engine, --local-previews renders them here also for an engine on this machine. --hidden never shows the window,
// has no Dock icon and never activates. --snapshot renders the window into a PNG after 8 seconds (or S) and quits,
// for evidence where screen recording is not permitted; --snapshots N takes N of them half a second apart
// (FILE-1.png ... FILE-N.png). --map opens the Map tab 3 seconds in: --map-screen opens the Nth screen large, --map-point selects
// corner K (0..3) of its first slice, --map-input switches to the Input view. --select-layer N selects layer N 3
// seconds in, so its Transform handles show on the monitor.
class Application : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Nocturne UI"; }
    const juce::String getApplicationVersion() override { return "0.1"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override
    {
        juce::StringArray args = juce::StringArray::fromTokens(commandLine, true);
        std::string host;
        int port = azl::kDefaultPort;
        juce::String snapshot;
        int mapScreen = 0, mapPoint = -1, selectLayer = 0, snapshots = 1, snapshotAfter = 8;
        const bool surfaces = !args.contains("--jpeg-previews");
        for (int i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "--host")
                host = args[i + 1].unquoted().toStdString();
            else if (args[i] == "--port")
                port = args[i + 1].getIntValue();
            else if (args[i] == "--snapshot")
                snapshot = args[i + 1].unquoted();
            else if (args[i] == "--map-screen")
                mapScreen = args[i + 1].getIntValue();
            else if (args[i] == "--map-point")
                mapPoint = args[i + 1].getIntValue();
            else if (args[i] == "--select-layer")
                selectLayer = args[i + 1].getIntValue();
            else if (args[i] == "--snapshots")
                snapshots = std::clamp(args[i + 1].getIntValue(), 1, 20);
            else if (args[i] == "--snapshot-after")
                snapshotAfter = std::clamp(args[i + 1].getIntValue(), 1, 600);
        }
        const bool hidden = args.contains("--hidden");
        if (!hidden && snapshot.isEmpty())
            host = azl::ui::rememberedHost(*azl::ui::MainComponent::defaultSettings(), host);
        else if (host.empty())
            host = "127.0.0.1";
        auto localRender = azl::ui::Session::LocalRender::Auto;
        if (args.contains("--remote-previews"))
            localRender = azl::ui::Session::LocalRender::Off;
        else if (args.contains("--local-previews"))
            localRender = azl::ui::Session::LocalRender::On;
        // The bundle starts as a background app, so a hidden run never flashes a Dock icon.
        if (!hidden)
            juce::Process::setDockIconVisible(true);
        const bool interactive = !hidden && snapshot.isEmpty();
        // The single-bundle app owns its engine: if nothing answers on the local port, start the nocturne engine that
        // sits beside this binary in Contents/MacOS (loopback only), and watch it (EngineKeepalive). It reuses an
        // engine that is already up and stops only one it started. A dev build with separate bundles has no engine
        // next to the UI, so this is a no-op there.
        if (interactive && isLoopbackHost(host)) {
            launcher_ = std::make_unique<azl::ui::EngineLauncher>(azl::ui::defaultEngineHooks(uint16_t(port)));
            launcher_->ensureRunning();
        }
        session_ = std::make_unique<azl::ui::Session>(
            host, uint16_t(port), surfaces, localRender, azl::ui::LocalPreviews::Options{},
            interactive ? azl::ui::DirectorMedia::defaults() : azl::ui::DirectorMedia::Options{});
        window_ = std::make_unique<Window>(*session_, !hidden);
        if (launcher_)
            keepalive_ = std::make_unique<EngineKeepalive>(*launcher_, *session_);
        if (interactive)
            session_->midi().start();
        if (!hidden && snapshot.isEmpty())
            session_->startVenues(azl::VenueSources::system(
                azl::loadTailscaleSettings(),
                [session = session_.get()](const azl::Venue& venue) { return session->tokenFor(venue.host); }));
        if (selectLayer > 0)
            juce::Timer::callAfterDelay(3000, [this, selectLayer] { session_->selectLayer(uint64_t(selectLayer)); });
        if (args.contains("--map"))
            juce::Timer::callAfterDelay(3000, [this, mapScreen, mapPoint, input = args.contains("--map-input")] {
                auto* main = static_cast<azl::ui::MainComponent*>(window_->getContentComponent());
                main->setMapMode(true);
                azl::ui::MapView& map = main->map();
                const auto& screens = session_->composition().screens;
                if (mapScreen > 0 && mapScreen <= int(screens.size()))
                    map.setFocus(screens[size_t(mapScreen - 1)].id);
                map.state().point = mapPoint;
                map.setInputView(input);
            });
        for (int i = 0; snapshot.isNotEmpty() && i < snapshots; ++i)
            juce::Timer::callAfterDelay(snapshotAfter * 1000 + 500 * i, [this, last = i + 1 == snapshots,
                                                         file = snapshots == 1 ? juce::File(snapshot)
                                                                               : snapshotFile(snapshot, i + 1)] {
                writeSnapshot(file);
                if (last)
                    quit();
            });
    }

    void shutdown() override
    {
        keepalive_.reset();
        window_.reset();
        session_.reset();
        if (launcher_)
            launcher_->shutdown();
    }

    void systemRequestedQuit() override { quit(); }

private:
    // True when the host names this machine over loopback, so starting a local engine is the right thing.
    static bool isLoopbackHost(const std::string& host)
    {
        std::string lower = host;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return lower.empty() || lower == "localhost" || lower == "127.0.0.1" || lower == "::1";
    }

    // shot.png -> shot-2.png
    static juce::File snapshotFile(const juce::String& path, int n)
    {
        const juce::File file(path);
        return file.getSiblingFile(file.getFileNameWithoutExtension() + "-" + juce::String(n) + file.getFileExtension());
    }

    void writeSnapshot(const juce::File& file)
    {
        juce::Component* content = window_->getContentComponent();
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(content->createComponentSnapshot(content->getLocalBounds(), true, 1.0f),
                                                  out);
        out.flush();
    }

    class Window : public juce::DocumentWindow {
    public:
        Window(azl::ui::Session& session, bool visible)
            : DocumentWindow("Nocturne", azl::ui::Look::palette().background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new azl::ui::MainComponent(session, azl::ui::MainComponent::defaultSettings()), true);
            setResizable(true, false);
            setResizeLimits(1100, 700, 10000, 10000);
            centreWithSize(getWidth(), getHeight());
            setVisible(visible);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<azl::ui::EngineLauncher> launcher_;
    std::unique_ptr<EngineKeepalive> keepalive_;
    std::unique_ptr<azl::ui::Session> session_;
    std::unique_ptr<Window> window_;
};

} // namespace

START_JUCE_APPLICATION(Application)
