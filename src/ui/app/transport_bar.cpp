#include "ui/app/transport_bar.hpp"

#include "protocol/build_info.hpp"
#include "ui/app/look.hpp"
#include "ui/app/midi.hpp"
#include "ui/app/widgets.hpp"

#include <algorithm>
#include <cmath>

namespace azl::ui {

namespace {

// Combo item ids are the quantisation in beats plus one.
const std::pair<const char*, int> kQuantise[] = {{"Q: Off", 0}, {"Q: Beat", 1}, {"Q: 1/2 Bar", 2},
                                                  {"Q: Bar", 4},  {"Q: 2 Bars", 8}, {"Q: 4 Bars", 16}};
const char* const kSections[] = {"Intro", "Build", "Drop", "Breakdown", "Outro"};
constexpr double kNudgePercent = 5.0;
constexpr int kGuardWidth = 190;
constexpr int kVenueWidth = 104;
constexpr int kRecordAudioWidth = 170;
constexpr int kSectionWidths[] = {44, 44, 44, 72, 48};
constexpr int kMinorGap = 6;
constexpr int kGroupGap = 12;
constexpr int kStatusFull = 236;
constexpr int kStatusDot = 120;
constexpr int kDocWidth = 110;

} // namespace

TransportBar::TransportBar(Session& session) : session_(session), venue_(session)
{
    for (juce::TextButton* button : {&perform_, &map_, &machines_, &slower_, &faster_, &halve_, &double_, &tap_,
                                     &resync_, &record_, &previews_, &settingsButton_, &prepare_})
        addAndMakeVisible(button);
    for (juce::TextButton* button : {&sendNow_, &sendAtSwitch_, &discard_})
        addChildComponent(button);
    prepare_.getProperties().set("accent", true);
    prepare_.setTooltip("Prepare: edit a copy of the venue's show and send it in one step; triggers and the clock "
                        "still go live. Off is Live: every edit goes out at once. With edits prepared, leave by Send "
                        "or Discard");
    sendNow_.setTooltip("Send the prepared change now, as one undo step");
    sendAtSwitch_.setTooltip("Send the prepared change at the next deck switch, also with this UI gone");
    discard_.setTooltip("Drop the prepared change; the venue never saw it");
    prepare_.onClick = [this] {
        EngineClient& client = session_.client();
        if (!client.preparing())
            client.prepare();
        else if (client.history().undo == 0)
            client.discardPrepared();
    };
    sendNow_.onClick = [this] { session_.client().sendPrepared(false); };
    sendAtSwitch_.onClick = [this] { session_.client().sendPrepared(true); };
    discard_.onClick = [this] { session_.client().discardPrepared(); };
    slower_.setTooltip("Hold to run the beat clock slower");
    faster_.setTooltip("Hold to run the beat clock faster");
    perform_.setTooltip("Perform: the clip grid, the monitors and the effect panes");
    map_.setTooltip("Map: where each output's slices take and put the composition");
    machines_.setTooltip("Machines: the venue machine's health, its outputs and its audio");
    halve_.setTooltip("Halve the tempo (down to " + juce::String(kMinBpm, 0) + " BPM)");
    double_.setTooltip("Double the tempo (up to " + juce::String(kMaxBpm, 0) + " BPM)");
    tap_.setTooltip("Tap on each beat to set the tempo");
    resync_.setTooltip("Restart the bar now: this beat becomes beat one");
    quantise_.setTooltip("Quantise: clip and column triggers wait for the next beat, half bar, bar or bars");
    for (juce::TextButton* button : {&slower_, &faster_})
        button->onStateChange = [this, button] {
            const bool down = button->isDown();
            if (down == button->getToggleState())
                return;
            button->setToggleState(down, juce::dontSendNotification);
            session_.client().nudge(down ? (button == &faster_ ? kNudgePercent : -kNudgePercent) : 0.0);
        };
    halve_.onClick = [this] { session_.client().edit(SetTempo{std::max(kMinBpm, session_.composition().bpm / 2.0)}); };
    double_.onClick = [this] { session_.client().edit(SetTempo{std::min(kMaxBpm, session_.composition().bpm * 2.0)}); };
    for (int i = 0; i < 5; ++i) {
        sections_[size_t(i)] = std::make_unique<juce::TextButton>(kSections[i]);
        juce::TextButton& button = *sections_[size_t(i)];
        button.getProperties().set("accent", true);
        button.setTooltip(juce::String("Start ") + kSections[i] + " on the next bar and fire its Cues");
        button.onClick = [this, i] { session_.client().markSection(juce::String(kSections[i]).toLowerCase().toStdString()); };
        addAndMakeVisible(button);
    }
    sectionMenu_.getProperties().set("accent", true);
    sectionMenu_.setTooltip("The Sections: start one on the next bar and fire its Cues");
    sectionMenu_.onClick = [this] { showMenu(sectionMenu_, sectionMenu()); };
    addChildComponent(sectionMenu_);
    perform_.setToggleState(true, juce::dontSendNotification);
    for (Tab tab : {Tab::Perform, Tab::Map, Tab::Machines})
        tabButton(tab).onClick = [this, tab] {
            setTab(tab);
            if (onTab)
                onTab(tab);
        };
    // A right-click opens their MIDI menu instead.
    tap_.onClick = [this] {
        if (!juce::ModifierKeys::currentModifiers.isPopupMenu())
            session_.client().tap();
    };
    resync_.onClick = [this] {
        if (!juce::ModifierKeys::currentModifiers.isPopupMenu())
            session_.client().resync();
    };
    tap_.addMouseListener(this, false);
    resync_.addMouseListener(this, false);
    record_.onClick = [this] { session_.client().record(!session_.client().record().recording); };
    settingsButton_.setTooltip(juce::String::fromUTF8("Settings: where and how the engine records (\xe2\x8c\x98,)"));
    settingsButton_.onClick = [this] { openSettings(); };
    previews_.setTooltip("Previews on the monitors and the Map; off spares the engine machine rendering them");
    previews_.onClick = [this] {
        session_.setPreviewsOn(!session_.previewsOn());
        if (onPreviews)
            onPreviews(session_.previewsOn());
    };
    for (const auto& [name, beats] : kQuantise)
        quantise_.addItem(name, beats + 1);
    quantise_.onChange = [this] {
        const int beats = quantise_.getSelectedId() - 1;
        if (beats >= 0 && beats != session_.composition().quantise)
            session_.client().edit(SetQuantise{beats});
    };
    addAndMakeVisible(quantise_);
    recordAudio_.setFont(Look::font());
    recordAudio_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(recordAudio_);
    venue_.onClick = [this] { openVenues(); };
    addAndMakeVisible(venue_);
    session_.addView(this);
    sessionChanged(Session::Document | Session::Record | Session::Previews);
}

TransportBar::~TransportBar()
{
    session_.removeView(this);
}

void TransportBar::setTab(Tab shown)
{
    for (Tab tab : {Tab::Perform, Tab::Map, Tab::Machines})
        tabButton(tab).setToggleState(tab == shown, juce::dontSendNotification);
}

int TransportBar::currentSection() const
{
    const double beat = session_.client().beatNow();
    int section = -1;
    for (const SectionMark& mark : session_.composition().timeline.sections)
        if (mark.beat <= beat + 1e-6)
            section = int(mark.section);
    return section;
}

juce::String TransportBar::guardText() const
{
    const uint64_t level = session_.client().clock().budgetLevel;
    if (level == 0)
        return {};
    return "guard: " + juce::String(int64_t(level)) + (level == 1 ? " layer" : " layers") + " half-res";
}

juce::String TransportBar::displayText() const
{
    const std::vector<OutputDisplay> displays = session_.client().displays();
    juce::StringArray flags;
    for (const Screen& screen : session_.composition().screens) {
        if (screen.display.empty())
            continue;
        auto it = std::find_if(displays.begin(), displays.end(),
                               [&](const OutputDisplay& d) { return d.id == screen.display; });
        if (it == displays.end())
            flags.add(utf8(screen.name) + ": display missing");
        else if (!it->mirrorOf.empty())
            flags.add(utf8(screen.name) + ": display mirrored");
    }
    return flags.joinIntoString(juce::String::fromUTF8(" \xc2\xb7 "));
}

std::pair<juce::String, juce::Colour> TransportBar::statusText() const
{
    const Palette& p = Look::palette();
    if (session_.status().isNotEmpty())
        return {session_.status(), session_.statusIsError() ? p.fault : p.text};
    const juce::String where = utf8(session_.host()) + ":" + juce::String(session_.port());
    if (!session_.client().connected()) {
        if (session_.engineError().isNotEmpty())
            return {session_.engineError(), p.fault};
        juce::String text = "connecting to " + where;
        if (!session_.connectionError().empty())
            text << ": " << utf8(session_.connectionError());
        return {text, p.accent};
    }
    if (const std::string skew = session_.client().versionSkew(); !skew.empty())
        return {utf8(skew), p.fault};
    if (const juce::String displays = displayText(); displays.isNotEmpty())
        return {displays, p.fault};
    if (session_.client().preparing())
        return {"Prepare: editing a copy, the venue is unchanged until Send", p.accent};
    if (const std::string warning = versionWarning(session_.client().engineBuild(), buildId()); !warning.empty())
        return {utf8(warning), p.accent};
    const juce::String engine = session_.client().local() ? "local engine " : "remote engine ";
    const juce::String change = juce::String(int64_t(session_.client().change()));
    return {engine + where + juce::String::fromUTF8(" \xc2\xb7 change ") + change, p.muted};
}

juce::String TransportBar::documentName() const
{
    const std::string path = session_.client().savedPath();
    return path.empty() ? juce::String("Untitled") : juce::File(utf8(path)).getFileNameWithoutExtension();
}

void TransportBar::save()
{
    if (const std::string path = session_.client().savedPath(); !path.empty())
        return session_.client().saveComposition(path);
    saveAs();
}

void TransportBar::saveAs()
{
    browse(true);
}

void TransportBar::open()
{
    browse(false);
}

void TransportBar::browse(bool saving)
{
    juce::Component* window = getTopLevelComponent();
    browser_ = std::make_unique<ShowBrowser>(session_, saving);
    window->addAndMakeVisible(*browser_);
    browser_->setBounds(window->getLocalBounds());
    if (browser_->isShowing())
        (saving ? browser_->pathField() : browser_->searchField()).grabKeyboardFocus();
}

void TransportBar::openSettings()
{
    juce::Component* window = getTopLevelComponent();
    settings_ = std::make_unique<SettingsModal>(session_);
    window->addAndMakeVisible(*settings_);
    settings_->setBounds(window->getLocalBounds());
}

void TransportBar::openVenues()
{
    juce::Component* window = getTopLevelComponent();
    venues_ = std::make_unique<VenueMenu>(session_);
    window->addAndMakeVisible(*venues_);
    venues_->setBounds(window->getLocalBounds());
}

Menu TransportBar::sectionMenu()
{
    Menu menu;
    const int current = currentSection();
    for (int i = 0; i < 5; ++i) {
        const std::string name = juce::String(kSections[i]).toLowerCase().toStdString();
        menu.push_back({.label = kSections[i],
                        .run = [this, name] { session_.client().markSection(name); },
                        .ticked = i == current});
    }
    return menu;
}

void TransportBar::sessionChanged(int changes)
{
    const int section = currentSection();
    for (int i = 0; i < 5; ++i)
        sections_[size_t(i)]->setToggleState(i == section, juce::dontSendNotification);
    sectionMenu_.setButtonText(section < 0 ? juce::String("Sections") : juce::String(kSections[section]));
    if (changes & Session::Interface)
        resized();
    if (changes & (Session::Document | Session::Connection)) {
        quantise_.setSelectedId(session_.composition().quantise + 1, juce::dontSendNotification);
        const bool connected = session_.client().connected();
        const bool preparing = session_.client().preparing();
        prepare_.setToggleState(preparing, juce::dontSendNotification);
        prepare_.setEnabled(connected);
        if (preparing != sendNow_.isVisible()) {
            for (juce::TextButton* button : {&sendNow_, &sendAtSwitch_, &discard_})
                button->setVisible(preparing);
            resized();
        }
    }
    if (changes & (Session::Record | Session::Connection)) {
        const RecordState record = session_.client().record();
        record_.setEnabled(session_.client().connected());
        record_.setToggleState(record.recording, juce::dontSendNotification);
        showRecordSpace(record);
        showRecordAudio(record);
    }
    if (changes & Session::Previews)
        previews_.setToggleState(session_.previewsOn(), juce::dontSendNotification);
    if (changes != Session::Clock || !beats_.isEmpty())
        repaint();
}

Menu TransportBar::midiMenu(bool tap)
{
    Menu menu;
    addMidiEntries(menu, session_, {.kind = tap ? MidiTarget::Kind::Tap : MidiTarget::Kind::Resync});
    return menu;
}

void TransportBar::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && (e.eventComponent == &tap_ || e.eventComponent == &resync_))
        showMenu(*e.eventComponent, midiMenu(e.eventComponent == &tap_));
}

struct TransportBar::Slot {
    juce::Component* component;
    juce::Rectangle<int>* rect;
    int width;
    int gapBefore;
};

// What the bar shows at a width: the recording audio label, Previews, the beat, the Section buttons and the nudge.
struct TransportBar::Plan {
    bool audio = true, previews = true, beats = true, sections = true, nudge = true;
};

int TransportBar::layout(const Plan& plan, bool place)
{
    const bool transport = session_.barGroupShown(Session::BarGroup::Transport);
    const bool sectionsOn = session_.barGroupShown(Session::BarGroup::Sections);
    const bool recording = session_.barGroupShown(Session::BarGroup::Recording);
    const bool preparing = sendNow_.isVisible();

    std::vector<std::vector<Slot>> groups;
    groups.push_back({{&venue_, nullptr, kVenueWidth, 0},
                      {&perform_, nullptr, 62, kMinorGap},
                      {&map_, nullptr, 42, 1},
                      {&machines_, nullptr, 66, 1},
                      {&prepare_, nullptr, 58, kMinorGap}});
    if (preparing)
        for (Slot slot :
             {Slot{&sendNow_, nullptr, 44, 1}, Slot{&sendAtSwitch_, nullptr, 64, 1}, Slot{&discard_, nullptr, 56, 1}})
            groups.back().push_back(slot);
    if (transport) {
        groups.push_back({{nullptr, &bpm_, 84, 0}});
        auto& g = groups.back();
        if (plan.nudge)
            g.insert(g.end(), {{&slower_, nullptr, 22, 1}, {&faster_, nullptr, 22, 1}});
        g.insert(g.end(), {{&halve_, nullptr, 36, 1},
                           {&double_, nullptr, 36, 1},
                           {&tap_, nullptr, 38, 1},
                           {&resync_, nullptr, 52, 1}});
        if (plan.beats)
            g.push_back({nullptr, &beats_, 60, 1});
        g.push_back({&quantise_, nullptr, 84, 1});
    }
    if (sectionsOn) {
        groups.emplace_back();
        if (plan.sections)
            for (int i = 0; i < 5; ++i)
                groups.back().push_back({sections_[size_t(i)].get(), nullptr, kSectionWidths[i], i == 0 ? 0 : 1});
        else
            groups.back().push_back({&sectionMenu_, nullptr, 84, 0});
    }
    groups.emplace_back();
    if (recording) {
        groups.back().push_back({&record_, nullptr, 46, 0});
        if (plan.audio)
            groups.back().push_back({&recordAudio_, nullptr, kRecordAudioWidth, 1});
    }
    if (plan.previews)
        groups.back().push_back({&previews_, nullptr, 60, recording ? kMinorGap : 0});
    groups.back().push_back({&settingsButton_, nullptr, 66, groups.back().empty() ? 0 : 1});

    int x = 0;
    const int height = getHeight();
    bool first = true;
    for (const auto& group : groups) {
        bool inGroup = false;
        for (const Slot& slot : group) {
            x += first ? 0 : inGroup ? slot.gapBefore : kGroupGap;
            first = false;
            inGroup = true;
            if (place) {
                const juce::Rectangle<int> r(x, 0, slot.width, height);
                if (slot.component == &recordAudio_)
                    recordAudio_.setBounds(r.withSizeKeepingCentre(r.getWidth(), 18));
                else if (slot.component)
                    slot.component->setBounds(r);
                if (slot.rect)
                    *slot.rect = r;
            }
            x += slot.width;
        }
    }
    return x;
}

void TransportBar::resized()
{
    static const Plan kPlans[] = {{true, true, true, true, true},     {false, true, true, true, true},
                                  {false, false, true, true, true},   {false, false, false, true, true},
                                  {false, false, false, false, true}, {false, false, false, false, false}};
    const Plan* chosen = &kPlans[std::size(kPlans) - 1];
    for (const Plan& plan : kPlans)
        if (layout(plan, false) + kGroupGap + kDocWidth + kStatusFull <= getWidth()) {
            chosen = &plan;
            break;
        }
    const bool recording = session_.barGroupShown(Session::BarGroup::Recording);
    const bool sections = session_.barGroupShown(Session::BarGroup::Sections);
    const bool transport = session_.barGroupShown(Session::BarGroup::Transport);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&record_, &recordAudio_})
        c->setVisible(recording && (c == &record_ || chosen->audio));
    for (auto& button : sections_)
        button->setVisible(sections && chosen->sections);
    sectionMenu_.setVisible(sections && !chosen->sections);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&halve_, &double_, &tap_, &resync_, &quantise_})
        c->setVisible(transport);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&slower_, &faster_})
        c->setVisible(transport && chosen->nudge);
    previews_.setVisible(chosen->previews);
    bpm_ = beats_ = {};
    const int right = layout(*chosen, true);
    status_ = getLocalBounds().withLeft(std::min(getWidth(), right + kGroupGap));
    doc_ = {};
    if (status_.getWidth() >= kDocWidth + kStatusFull)
        doc_ = status_.removeFromLeft(kDocWidth);
    repaint();
}

namespace {

juce::String stopReason(const RecordState& record)
{
    if (record.stopsAtClock.empty())
        return {};
    const juce::String why = record.stopsFor == "gig over" ? "after the gig" : "at its maximum length";
    return "It finishes at " + utf8(record.stopsAtClock) + ", " + why + ". ";
}

} // namespace

void TransportBar::showRecordSpace(const RecordState& record)
{
    const bool low = record.recording && record.space == "low";
    const bool full = !record.recording && record.space == "full";
    record_.getProperties().set("accent", low);
    record_.getProperties().set("fault", full);
    const juce::String space = juce::String(double(record.freeMB) / 1024.0, 1) + " GB free, the recording stops below " +
                               juce::String(double(record.stopsAtMB) / 1024.0, 1) + " GB";
    if (low)
        record_.setTooltip("Recording " + utf8(record.target) + " to " + utf8(record.file) + ". Disk space low: " + space +
                           ". Click to stop.");
    else if (record.recording)
        record_.setTooltip("Recording " + utf8(record.target) + " as " + utf8(record.codec) + " to " + utf8(record.file) +
                           ". " + stopReason(record) + "Click to stop.");
    else if (full)
        record_.setTooltip("Recording stopped: the disk is full. Click to record again once there is space.");
    else
        record_.setTooltip("Record the composition with the engine's settings");
}

void TransportBar::showRecordAudio(const RecordState& record)
{
    const Palette& p = Look::palette();
    const bool missing = record.audio == "unavailable";
    juce::String text, tooltip;
    if (record.audio == "on") {
        text = "audio: " + utf8(record.audioSource);
        tooltip = "The recording's audio track: " + utf8(record.audioSource);
    } else if (record.audio == "off") {
        text = "audio off";
        tooltip = "Recordings have no audio track (record audio on to add one)";
    } else if (missing) {
        text = "no audio: " + utf8(record.audioReason);
        tooltip = (record.recording ? "Recording without audio: " : "A recording now would have no audio: ") +
                  utf8(record.audioReason);
    }
    if (record.recording && !record.stopsAtClock.empty() && !missing) {
        text = "stops at " + utf8(record.stopsAtClock);
        tooltip = stopReason(record) + tooltip;
    }
    recordAudio_.setText(text, juce::dontSendNotification);
    recordAudio_.setTooltip(tooltip);
    recordAudio_.setColour(juce::Label::textColourId, missing ? p.accent : p.muted);
}

void TransportBar::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.line);
    g.setFont(Look::font());
    if (!doc_.isEmpty()) {
        const bool unsaved = session_.client().unsaved();
        g.setColour(p.muted);
        drawText(g, documentName(), doc_.withTrimmedLeft(8).withTrimmedRight(unsaved ? 20 : 8),
                 juce::Justification::centredLeft);
        if (unsaved) {
            g.setColour(p.accent);
            g.fillEllipse(
                juce::Rectangle<float>(7.0f, 7.0f).withCentre({float(doc_.getRight() - 10), float(doc_.getCentreY())}));
        }
    }

    if (!bpm_.isEmpty()) {
        g.setColour(juce::Colour(0xff1d1d1d));
        g.fillRect(bpm_);
        g.setColour(p.muted);
        g.setFont(Look::font());
        drawText(g, "BPM", bpm_.withTrimmedLeft(9), juce::Justification::centredLeft);
        g.setColour(p.white);
        g.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
        drawText(g, juce::String(session_.composition().bpm, 2), bpm_.withTrimmedRight(9),
                 juce::Justification::centredRight);
    }

    if (!beats_.isEmpty())
        paintBeats(g);

    paintStatus(g);
}

void TransportBar::paintBeats(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setColour(juce::Colour(0xff1d1d1d));
    g.fillRect(beats_);
    const bool clocked = session_.client().connected();
    const int beat = clocked ? int(std::floor(session_.client().beatNow())) : -1;
    for (int i = 0; i < 4; ++i) {
        auto square = juce::Rectangle<int>(beats_.getX() + 8 + i * 13, beats_.getCentreY() - 5, 11, 11);
        const bool on = beat >= 0 && ((beat % 4) + 4) % 4 == i;
        g.setColour(on ? (i == 0 ? p.fault : p.accent) : juce::Colour(0xff333333));
        g.fillRect(square);
    }
}

void TransportBar::paintStatus(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const auto status = statusArea();
    g.setColour(p.bar);
    g.fillRect(status);
    const auto [text, ink] = statusText();
    if (status.getWidth() < kStatusDot) {
        g.setColour(ink);
        g.fillEllipse(juce::Rectangle<float>(8.0f, 8.0f).withCentre(status.getCentre().toFloat()));
        return;
    }
    g.setFont(Look::font());
    auto textArea = status.reduced(9, 0);
    if (const juce::String guard = guardText(); guard.isNotEmpty()) {
        g.setColour(p.accent);
        drawText(g, guard, textArea.removeFromRight(kGuardWidth), juce::Justification::centredRight, true);
    }
    g.setColour(ink);
    drawText(g, text, textArea, juce::Justification::centredRight, true);
}

} // namespace azl::ui
