#include "net/websocket.hpp"
#include "test_client.hpp"
#include "ui/app/main_component.hpp"

#include <functional>

namespace {

using azl::ui::TipTiming;

bool runUntil(azl::EngineHarness& harness, const std::function<bool()>& done, double seconds = 20.0)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        harness.step();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(15);
        if (done())
            return true;
    }
    return false;
}

juce::String describe(juce::Component& c)
{
    juce::String name = typeid(c).name();
    if (auto* button = dynamic_cast<juce::Button*>(&c))
        name << " '" << button->getButtonText() << "'";
    return name << " " << c.getName() << " at " << c.getScreenBounds().toString();
}

bool passive(juce::Component& c)
{
    bool clicks = false, childClicks = false;
    c.getInterceptsMouseClicks(clicks, childClicks);
    return !clicks || dynamic_cast<juce::Label*>(&c) || dynamic_cast<juce::ScrollBar*>(&c) ||
           dynamic_cast<juce::TooltipWindow*>(&c);
}

// Parts of a JUCE control or viewport, and the app's own components that only lay out others.
bool layout(juce::Component& c)
{
    using namespace azl::ui;
    return !juce::String(typeid(c).name()).contains("azl") || dynamic_cast<ClipGrid*>(&c) ||
           dynamic_cast<ChainContent*>(&c) || dynamic_cast<OwnerCard*>(&c) || dynamic_cast<AnimateBox*>(&c) ||
           dynamic_cast<TransportBar*>(&c) || dynamic_cast<PreviewMonitor*>(&c);
}

juce::String tipOf(juce::Component& c)
{
    if (auto* tips = dynamic_cast<azl::ui::PointerTips*>(&c)) {
        for (int y = 0; y < c.getHeight(); y += 3)
            for (int x = 0; x < c.getWidth(); x += 3)
                if (const juce::String tip = tips->tipAt({x, y}); tip.isNotEmpty())
                    return tip;
        return {};
    }
    if (auto* client = dynamic_cast<juce::TooltipClient*>(&c))
        return client->getTooltip();
    return layout(c) ? "-" : "";
}

// Every visible component under `root` a hand can use that says nothing about itself.
void silent(juce::Component& root, juce::StringArray& out)
{
    for (juce::Component* child : root.getChildren()) {
        if (!child->isVisible() || child->getBounds().isEmpty())
            continue;
        if (!passive(*child) && tipOf(*child).isEmpty())
            out.add(describe(*child));
        silent(*child, out);
    }
}

void checkSilent(juce::Component& root, const juce::String& where)
{
    juce::StringArray missing;
    silent(root, missing);
    INFO(where.toStdString() << ":\n" << missing.joinIntoString("\n").toStdString());
    CHECK(missing.isEmpty());
}

} // namespace

TEST_CASE("tooltips: a tip shows after a 1 s rest, never while a button is down and not within 1 s of a click")
{
    TipTiming timing;
    CHECK(timing.filter("Tap", 10000).isEmpty());
    CHECK(timing.filter("Tap", 10999).isEmpty());
    CHECK(timing.filter("Tap", 11000) == "Tap");
    // A new tip under the pointer starts its own rest.
    CHECK(timing.filter("Resync", 11100).isEmpty());
    CHECK(timing.filter("Resync", 12100) == "Resync");

    // A click hides it and keeps it away for a second after the release.
    timing.pressed(13000);
    CHECK(timing.filter("Resync", 14500).isEmpty());
    timing.released(14600);
    CHECK(timing.filter("Resync", 15000).isEmpty());
    CHECK(timing.filter("Resync", 15600) == "Resync");

    // A drag never shows one, however long it rests.
    timing.pressed(20000);
    CHECK(timing.filter("Opacity", 20100).isEmpty());
    CHECK(timing.filter("Opacity", 25000).isEmpty());
    timing.released(25000);
    CHECK(timing.filter("Opacity", 25999).isEmpty());
    CHECK(timing.filter("Opacity", 26000) == "Opacity");
}

TEST_CASE("tooltips: every visible control in every pane says what it does")
{
    TempDir media;
    azl::EngineHarness harness{{.width = 960, .height = 540}};
    Client client{harness.document()};
    const uint64_t base = 1;
    const Message added = addMedia(client, copyMedia("count_100.mp4", media.path / "loops" / "count_100.mp4"));
    const azl::ClipRef clip{added["entry"]["hash"], added["entry"]["name"]};
    const auto columns = harness.document().document().decks[0].columns;
    apply(client, azl::SetSlot{base, columns[0].id, clip});
    const uint64_t reveal =
        apply(client, azl::AddEffect{base, std::nullopt, {0, "reveal", {{"black", 0.6}}}})["effect"]["id"];
    azl::Modulator kick;
    kick.source = azl::ModulatorSource::Audio;
    kick.target.macro = 1;
    const azl::Macro energy{.id = 1, .name = "Energy", .targets = {{{0, reveal, "evolve"}, {}, 0.0, 2.0}}};
    apply(client, azl::SetMacros{{energy}, {kick}});
    azl::Cue cue;
    cue.section = azl::Section::Drop;
    cue.macros.push_back({1, 1.0, 0});
    apply(client, azl::AddCue{std::nullopt, cue});
    client.send({{"type", "trigger"}, {"op", "column"}, {"column", columns[0].id}});
    harness.step(4);
    client.send({{"type", "modulation"}, {"op", "save"}, {"name", "Techno"}});

    azl::WebSocketServer server(harness.document(), 0);
    azl::ui::Session session("127.0.0.1", server.port());
    azl::ui::MainComponent window(session);
    window.setSize(1600, 950);
    REQUIRE(runUntil(harness, [&] { return session.client().synced(); }));
    session.client().requestLibrary();
    REQUIRE(runUntil(harness, [&] { return session.client().library().size() == 1; }));

    bool timed = false;
    for (juce::Component* child : window.getChildren())
        timed |= dynamic_cast<azl::ui::Tooltips*>(child) != nullptr;
    CHECK(timed);

    session.selectLayer(base);
    azl::ui::ChainContent& chain = window.chain().content();
    REQUIRE(runUntil(harness, [&] { return chain.card(reveal) != nullptr; }));
    session.animate({0, reveal, "black"}, "bass");
    REQUIRE(runUntil(harness, [&] { return harness.document().document().modulators.size() == 2; }));
    session.selectClip({base, columns[0].id});
    REQUIRE(runUntil(harness, [&] { return window.mid().macros().targetRow(0) != nullptr; }));

    for (int chainTab = 0; chainTab < 3; ++chainTab) {
        session.setChainTab(chainTab);
        runUntil(harness, [] { return false; }, 0.3);
        for (int midTab = 0; midTab < 4; ++midTab) {
            window.mid().showTab(midTab);
            for (int browserTab = 0; browserTab < 3; ++browserTab) {
                window.browser().selectTab(browserTab);
                runUntil(harness, [] { return false; }, 0.1);
                checkSilent(window, "chain tab " + juce::String(chainTab) + ", mid tab " + juce::String(midTab) +
                                        ", browser tab " + juce::String(browserTab));
            }
        }
    }

    // Values name their unit and range; the two ends of a range say which is which.
    session.setChainTab(1);
    window.mid().showMacros();
    REQUIRE(runUntil(harness, [&] { return chain.card(reveal) && chain.card(reveal)->row("black")->animateBox(); }));
    azl::ui::ParamRow& black = *chain.card(reveal)->row("black");
    CHECK(black.tipAt(black.barArea().getCentre()).contains("0% to 100%"));
    CHECK(black.tipAt({*black.handleX(0), black.barArea().getCentreY()}).startsWith("From"));
    CHECK(black.tipAt({*black.handleX(1) - 2, black.barArea().getCentreY()}).startsWith("To"));
    CHECK(black.animateBox()->toField().getTooltip().contains("0% to 100%"));
    azl::ui::TargetRow& target = *window.mid().macros().targetRow(0);
    CHECK(target.fromField().getTooltip().startsWith("From"));
    CHECK(target.toField().getTooltip().startsWith("To"));
    CHECK(target.toField().getTooltip().contains("0.00 to 4.00"));
    CHECK(window.mid().macros().modulatorRow(0)->envelope().attackField().getTooltip().contains("ms, 0 to 10000"));

    REQUIRE(runUntil(harness, [&] { return !session.client().macroSets().empty(); }));
    window.mid().macros().loadButton().triggerClick();
    REQUIRE(runUntil(harness, [&] { return window.mid().macros().setPreview() != nullptr; }));
    checkSilent(*window.mid().macros().setPreview(), "Macro Set preview");

    window.transport().open();
    REQUIRE(window.transport().browser() != nullptr);
    runUntil(harness, [] { return false; }, 0.3);
    checkSilent(*window.transport().browser(), "show browser");
}
