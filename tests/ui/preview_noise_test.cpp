#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/session.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <random>
#include <thread>

namespace {

using azl::ClipRef;
using azl::Effect;
using azl::ui::Session;
using Clock = std::chrono::steady_clock;

// The size the UI watches at, so a preview is the engine's picture pixel for pixel.
constexpr azl::EngineConfig kSize{.width = azl::ui::EngineClient::kSurfaceWidth,
                                  .height = azl::ui::EngineClient::kSurfaceHeight};

ClipRef solid(double red, double green, double blue)
{
    return {.name = "solid", .effects = {Effect{0, "solid", {{"red", red}, {"green", green}, {"blue", blue}}}}};
}

uint64_t revealOf(const azl::Composition& composition, uint64_t layer)
{
    uint64_t id = 0;
    forEachLayer(composition.layers, [&](const azl::Layer& l) {
        for (const Effect& e : l.effects)
            if (l.id == layer && e.type == "reveal")
                id = e.id;
    });
    return id;
}

// The user's Reveal on the layer, its evolve driven by an LFO.
void addReveal(Client& editor, azl::EngineHarness& harness, uint64_t layer, double beats)
{
    apply(editor, azl::AddEffect{layer, std::nullopt,
                                 Effect{0, "reveal",
                                        {{"progress", 0.77}, {"columns", 8.0}, {"rows", 8.0}, {"gap", 0.05},
                                         {"seed", 33.0}, {"reshuffle", 0.0}, {"softness", 0.05}, {"evolve", 3.0}}}});
    const uint64_t reveal = revealOf(*harness.engine().composition(), layer);
    REQUIRE(reveal != 0);
    apply(editor, azl::AddModulator{std::nullopt, azl::Modulator{.beats = beats,
                                                                 .target = {0, azl::ParamRef{layer, reveal, "evolve"}},
                                                                 .min = 0.0,
                                                                 .max = 1.0}});
}

// The first pixel not `allowed`.
std::optional<std::pair<juce::Point<int>, juce::Colour>> outside(const juce::Image& image,
                                                                 const std::function<bool(juce::Colour)>& allowed)
{
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
            if (const juce::Colour c = image.getPixelAt(x, y); !allowed(c))
                return std::pair{juce::Point<int>(x, y), c};
    return std::nullopt;
}

// Hands the engine's messages to a client late, in order, as a busy UI or a backed up socket does.
class LateDelivery {
public:
    LateDelivery(azl::ui::EngineClient& client, int maxDelayMs) : client_(client), maxDelay_(maxDelayMs)
    {
        thread_ = std::thread([this] { run(); });
    }
    ~LateDelivery()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    void push(const Message& message)
    {
        std::lock_guard lock(mutex_);
        last_ = std::max(last_, Clock::now() + std::chrono::milliseconds(random_() % (maxDelay_ + 1)));
        queue_.emplace_back(last_, message);
        wake_.notify_all();
    }

private:
    void run()
    {
        std::unique_lock lock(mutex_);
        while (!stopping_) {
            if (queue_.empty()) {
                wake_.wait(lock);
                continue;
            }
            if (Clock::now() < queue_.front().first) {
                wake_.wait_until(lock, queue_.front().first);
                continue;
            }
            Message message = std::move(queue_.front().second);
            queue_.pop_front();
            lock.unlock();
            client_.receive(message);
            lock.lock();
        }
    }

    azl::ui::EngineClient& client_;
    int maxDelay_;
    std::mt19937 random_{11};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::pair<Clock::time_point, Message>> queue_;
    Clock::time_point last_{};
    bool stopping_ = false;
    std::thread thread_;
};

struct Pictures : azl::ui::EngineClient::Listener {
    std::function<void(const std::string&)> arrived;
    void previewArrived(const std::string& source) override { arrived(source); }
};

int maxDifference(const azl::ui::PreviewPicture& picture, const azl::Image& reference)
{
    if (picture.bgra.size() != reference.rgba.size())
        return 255;
    int worst = 0;
    for (size_t i = 0; i < picture.bgra.size(); i += 4)
        for (size_t c = 0; c < 3; ++c)
            worst = std::max(worst, std::abs(int(picture.bgra[i + 2 - c]) - int(reference.rgba[i + c])));
    return worst;
}

} // namespace

TEST_CASE("a preview read late from its shared surface is one whole picture of the frame it says, never a later or "
          "half-painted one")
{
    TempDir media;
    azl::EngineHarness harness{kSize, 30.0};
    Client editor{harness.document()};
    const Message added = addMedia(editor, copyMedia("count_240.mp4", media.path / "count.mp4"));
    apply(editor, azl::SetSlot{1, 1, ClipRef{added["entry"]["hash"], "clip", {}}});
    addReveal(editor, harness, 1, 1.0);
    editor.send({{"type", "trigger"}, {"op", "column"}, {"column", 1}});
    harness.step();

    std::mutex mutex;
    std::map<uint64_t, azl::Image> rendered{{harness.engine().latestFrame()->index, harness.readPicture(1)}};
    size_t shown = 0, wrong = 0;
    Pictures pictures;
    azl::ui::EngineClient client(pictures);
    LateDelivery late(client, 150);
    pictures.arrived = [&](const std::string& source) {
        const auto picture = client.preview(source);
        std::lock_guard lock(mutex);
        auto reference = picture ? rendered.find(picture->frame) : rendered.end();
        ++shown;
        if (reference == rendered.end() || maxDifference(*picture, reference->second) > 1)
            ++wrong;
    };
    const azl::ClientId id = harness.document().connect([&](const Message& message) { late.push(message); });
    client.attached([&](const Message& message) { harness.document().receive(id, message); }, true);
    client.watchPreviews({"layer:1"});

    for (int i = 0; i < 90; ++i) {
        {
            std::lock_guard lock(mutex);
            harness.step();
            rendered[harness.engine().latestFrame()->index] = harness.readPicture(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    client.watchPreviews({});
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    harness.document().disconnect(id);
    std::lock_guard lock(mutex);
    MESSAGE(shown << " pictures shown, " << wrong << " not the picture of their frame");
    CHECK(shown > 20);
    CHECK(wrong == 0);
}

TEST_CASE("for a minute of selections, column changes and previews toggled, every layer preview the UI shows holds "
          "only that layer's picture and keeps moving")
{
    azl::EngineHarness harness{kSize, 30.0};
    azl::WebSocketServer server{harness.document(), 0};
    Client editor{harness.document()};
    const azl::ClientId quiet = harness.document().connect([](const Message&) {});
    auto send = [&](const Message& message) { harness.document().receive(quiet, message); };
    const uint64_t top = apply(editor, azl::AddLayer{0, std::nullopt, azl::Layer{.name = "Top"}})["layer"]["id"];
    apply(editor, azl::SetSlot{1, 1, solid(1.0, 0.0, 0.0)});
    apply(editor, azl::SetSlot{1, 2, solid(0.0, 0.0, 1.0)});
    apply(editor, azl::SetSlot{top, 1, solid(0.0, 1.0, 0.0)});
    apply(editor, azl::SetSlot{top, 2, solid(0.0, 1.0, 0.0)});
    addReveal(editor, harness, 1, 4.0);
    send({{"type", "trigger"}, {"op", "column"}, {"column", 1}});
    harness.startLive(30.0);

    Session session("127.0.0.1", server.port(), true, Session::LocalRender::Off);
    REQUIRE_FALSE(session.localPreviews());
    auto pump = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil(5); };
    for (const auto end = Clock::now() + std::chrono::seconds(30); Clock::now() < end && !session.client().synced();)
        pump();
    REQUIRE(session.client().synced());

    // The base layer is red or blue where Reveal shows it and black elsewhere; the top layer only green.
    const std::string base = "layer:1", upper = "layer:" + std::to_string(top);
    const std::map<std::string, std::function<bool(juce::Colour)>> allowed{
        {base, [](juce::Colour c) { return c.getGreen() <= 1 && (c.getRed() <= 1 || c.getBlue() <= 1); }},
        {upper, [](juce::Colour c) { return c.getRed() <= 1 && c.getBlue() <= 1; }}};
    std::mt19937 random(7);
    size_t checked = 0, foreign = 0, stalls = 0;
    std::map<std::string, juce::Image> last;
    Clock::time_point baseShownSince = Clock::now(), baseChanged = Clock::now();
    bool baseShown = false;
    auto nextAction = Clock::now();
    for (const auto end = Clock::now() + std::chrono::seconds(60); Clock::now() < end;) {
        pump();
        if (Clock::now() >= nextAction) {
            nextAction += std::chrono::milliseconds(150 + random() % 300);
            switch (random() % 8) {
            case 0: session.selectLayer(1); break;
            case 1: session.selectLayer(top); break;
            case 2: session.selectClip({1, 1 + random() % 2}); break;
            case 3: session.selectClip({top, 1 + random() % 2}); break;
            case 4: send({{"type", "trigger"}, {"op", "column"}, {"column", 1 + random() % 2}}); break;
            case 5: session.setPreviewsOn(false); break;
            case 6: session.setPreviewsOn(true); break;
            case 7: session.selectComposition(); break;
            }
        }
        const bool showsBase = session.previewsOn() && session.previewSource() == base;
        if (showsBase && !baseShown)
            baseShownSince = baseChanged = Clock::now();
        baseShown = showsBase;
        for (const auto& [source, ok] : allowed) {
            const juce::Image image = session.preview(source);
            if (!image.isValid() || last[source] == image)
                continue;
            last[source] = image;
            if (source == base)
                baseChanged = Clock::now();
            ++checked;
            if (const auto bad = outside(image, ok)) {
                ++foreign;
                MESSAGE(source << " at " << bad->first.toString() << ": " << bad->second.toDisplayString(true));
            }
        }
        if (baseShown && Clock::now() - baseShownSince > std::chrono::seconds(1) &&
            Clock::now() - baseChanged > std::chrono::milliseconds(700)) {
            ++stalls;
            baseChanged = Clock::now();
        }
    }
    harness.stopLive();
    harness.document().disconnect(quiet);
    MESSAGE(checked << " pictures checked");
    CHECK(checked > 200);
    CHECK(foreign == 0);
    CHECK(stalls == 0);
}
