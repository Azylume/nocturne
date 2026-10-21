#include "ui/app/machines_view.hpp"

#include "ui/app/look.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

namespace azl::ui {

namespace {

constexpr int kMargin = 8;
constexpr int kHeader = 22;
constexpr int kRow = 20;
constexpr int kLabel = 86;
constexpr int kChip = 66;
constexpr int kScreenName = 110;
constexpr int kDetails = 46;
constexpr int kSidePanel = 300;
constexpr int kAudioPanel = 380;
constexpr int kOutputsPanel = 560;
constexpr int kButton = 24;
constexpr int kGap = 4;
constexpr int kActionRows = 3;
constexpr int kTileMaxWidth = 240;

const juce::String kDot = juce::String::fromUTF8(" \xc2\xb7 ");

juce::String orDash(const std::optional<double>& value, int decimals, const juce::String& unit = {})
{
    return value ? juce::String(*value, decimals) + unit : juce::String("-");
}

juce::String inTime(double seconds)
{
    const int64_t s = std::max<int64_t>(0, std::llround(seconds));
    if (s < 60)
        return "in " + juce::String(s) + " s";
    const juce::String minutes = juce::String(s / 60 % 60).paddedLeft('0', s >= 3600 ? 2 : 1);
    const juce::String rest = minutes + ":" + juce::String(s % 60).paddedLeft('0', 2);
    return "in " + (s >= 3600 ? juce::String(s / 3600) + ":" + rest : rest);
}

juce::Colour stateColour(const std::string& state)
{
    const Palette& p = Look::palette();
    if (state == "ok")
        return p.live;
    if (state == "low rate")
        return p.accent;
    return p.fault;
}

juce::String outputFigures(const MachineOutput& o)
{
    return utf8(o.displayName) + kDot + orDash(o.presents, 1, "/s") + kDot +
           (o.hitches ? juce::String(int64_t(*o.hitches)) + " hitches" : juce::String("-"));
}

juce::String outputLine(const MachineOutput& o)
{
    return utf8(o.screenName) + kDot + outputFigures(o) + kDot + utf8(o.state);
}

juce::String djPlayerLabel(const MachineStatus::DjPlayer& d)
{
    return "Player " + juce::String(d.number);
}

juce::String djPlayerFigures(const MachineStatus::DjPlayer& d)
{
    juce::StringArray parts;
    if (d.onAir)
        parts.add(*d.onAir ? "on air" : "off air");
    juce::String tempo = orDash(d.bpm, 2, " BPM");
    if (d.pitch)
        tempo << " " << (*d.pitch >= 0.0 ? "+" : "") << juce::String(*d.pitch, 2) << "%";
    parts.add(tempo);
    if (!d.play.empty())
        parts.add(utf8(d.play));
    if (d.beatInBar)
        parts.add(juce::String(*d.beatInBar) + "/4");
    return parts.joinIntoString(kDot);
}

int djRows(const MachineStatus& s)
{
    return s.djlinkEnabled.value_or(false) ? 1 + int(s.djPlayers.size()) : 0;
}

int tileWidth(int width, int count)
{
    return std::min(kTileMaxWidth, (width - kGap * (count - 1)) / count);
}

int outputHeight(const MachineOutput& o, const std::set<std::string>& expanded)
{
    return kRow + (expanded.contains(o.display) ? kDetails : 0);
}

void paintPanel(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title)
{
    const Palette& p = Look::palette();
    g.setColour(p.panel);
    g.fillRect(area);
    g.setFont(Look::font(true));
    fillWithText(g, area.removeFromTop(kHeader), p.header, p.white, title, juce::Justification::centredLeft, 8);
}

void paintRow(juce::Graphics& g, juce::Rectangle<int> row, const juce::String& label, const juce::String& value,
              juce::Colour ink)
{
    const Palette& p = Look::palette();
    row.removeFromLeft(8);
    g.setFont(Look::font());
    g.setColour(p.muted);
    drawText(g, label, row.removeFromLeft(kLabel), juce::Justification::centredLeft);
    g.setColour(ink);
    drawText(g, value, row.withTrimmedRight(8), juce::Justification::centredLeft);
}

void paintHistory(juce::Graphics& g, juce::Rectangle<int> area, const std::deque<double>& history, double top)
{
    const Palette& p = Look::palette();
    g.setColour(p.field);
    g.fillRect(area);
    if (history.size() < 2 || top <= 0.0)
        return;
    juce::Path line;
    const float step = float(area.getWidth()) / float(MachinesView::kHistory - 1);
    const float x0 = float(area.getRight()) - step * float(history.size() - 1);
    for (size_t i = 0; i < history.size(); ++i) {
        const float x = x0 + step * float(i);
        const float y = float(area.getBottom()) - float(std::clamp(history[i] / top, 0.0, 1.0)) * float(area.getHeight());
        if (i == 0)
            line.startNewSubPath(x, y);
        else
            line.lineTo(x, y);
    }
    g.setColour(p.meter);
    g.strokePath(line, juce::PathStrokeType(1.5f));
}

juce::String readinessText(const MachineStatus::Media& m)
{
    if (!m.missing)
        return "checking";
    if (m.ready)
        return "ready";
    juce::StringArray decks;
    for (const auto& [name, missing] : m.decks)
        decks.add(utf8(name) + " " + juce::String(missing));
    return juce::String(*m.missing) + " missing" + (decks.isEmpty() ? juce::String() : kDot + decks.joinIntoString(", "));
}

juce::String syncText(const MachineStatus::Media& m)
{
    if (m.state == "downloading")
        return "downloading" + (m.progress ? " " + juce::String(int(*m.progress * 100.0)) + "%" : juce::String());
    if (m.state == "waiting")
        return "waiting for the gig window to end";
    if (m.state == "blocked")
        return "blocked by hand";
    if (m.state == "error")
        return utf8(m.error);
    return utf8(m.state);
}

} // namespace

MachinesView::MachinesView(Session& session) : session_(session)
{
    session_.addView(this);
    auto send = [this](Message action) { session_.client().machineAction(std::move(action)); };
    testCard_.onClick = [this, send] { send({{"op", "testcard"}, {"on", !(status_ && status_->testCard)}}); };
    identify_.onClick = [send] { send({{"op", "identify"}, {"seconds", 3.0}}); };
    unmirror_.onClick = [send] { send({{"op", "unmirror"}}); };
    lock_.onClick = [send] { send({{"op", "lock"}, {"on", true}}); };
    restart_.onHold = [send] { send({{"op", "restart"}}); };
    safeMode_.onHold = [send] { send({{"op", "safeMode"}}); };
    unlock_.onHold = [send] { send({{"op", "lock"}, {"on", false}}); };
    update_.onHold = [this] { startUpdate(); };
    updateBundle_ = juce::File::getSpecialLocation(juce::File::currentApplicationFile)
                        .getSiblingFile("Nocturne.app")
                        .getFullPathName()
                        .toStdString();
    testCard_.setClickingTogglesState(false);
    identify_.setTooltip("Every output shows its screen's name and display large for 3 s");
    unmirror_.setTooltip("End macOS mirroring between displays that carry different screens now");
    lock_.setTooltip("Refuse Restart engine, Safe mode and Update from every client until it is turned off by holding");
    for (auto [button, mode, tip] : {std::tuple{&syncAuto_, "auto", "Download missing media only outside the gig window"},
                                     std::tuple{&syncAllow_, "allow", "Download missing media now, even during the gig"},
                                     std::tuple{&syncBlock_, "block", "Download nothing, and stop a download under way"}}) {
        button->onClick = [this, mode] { session_.client().setMediaSync(mode); };
        button->setTooltip(tip);
        button->setClickingTogglesState(false);
    }
    listen_.setClickingTogglesState(false);
    listen_.setTooltip("Hear this machine's audio input here, about half a second late");
    listen_.onClick = [this] {
        session_.setListening(!session_.listening());
        updateActions();
    };
    volume_.setRange(0.0, 1.0);
    volume_.setValue(session_.returnAudio().volume(), juce::dontSendNotification);
    volume_.setTooltip("Listen volume");
    volume_.onValueChange = [this] { session_.returnAudio().setVolume(volume_.getValue()); };
    for (juce::Component* c :
         std::initializer_list<juce::Component*>{&testCard_, &identify_, &unmirror_, &lock_, &restart_, &safeMode_,
                                                 &unlock_, &update_, &syncAuto_, &syncAllow_, &syncBlock_, &listen_,
                                                 &volume_})
        addChildComponent(c);
}

MachinesView::~MachinesView()
{
    session_.removeView(this);
    session_.client().watchMachine(false);
    session_.client().watchVideo(false);
}

void MachinesView::visibilityChanged()
{
    session_.client().watchMachine(isVisible());
    session_.client().watchVideo(isVisible());
}

void MachinesView::sessionChanged(int changes)
{
    if (changes & Session::Connection && !session_.client().connected()) {
        status_.reset();
        history_.clear();
        tiles_.clear();
    }
    if (changes & Session::Machine) {
        status_ = session_.client().machine();
        if (status_)
            for (const MachineOutput& o : status_->outputs) {
                std::deque<double>& history = history_[o.display];
                history.push_back(o.presents.value_or(0.0));
                if (history.size() > kHistory)
                    history.pop_front();
            }
    }
    if (changes & (Session::Machine | Session::Connection)) {
        updateActions();
        resized();
    }
    if (changes & (Session::Machine | Session::Connection | Session::Venues))
        repaint();
    if (changes & Session::ReturnVideo) {
        updateTiles();
        repaint(tilesArea());
    }
}

void MachinesView::updateTiles()
{
    if (!status_)
        return;
    std::map<std::string, juce::Image> tiles;
    for (const MachineOutput& o : status_->outputs) {
        const std::optional<VideoTile> tile = session_.client().videoTile(o.display);
        if (!tile || tile->picture.bgra.empty())
            continue;
        const H264Decoder::Picture& picture = tile->picture;
        juce::Image image(juce::Image::ARGB, int(picture.width), int(picture.height), false);
        juce::Image::BitmapData data(image, juce::Image::BitmapData::writeOnly);
        const size_t row = size_t(picture.width) * 4;
        for (uint32_t y = 0; y < picture.height; ++y)
            std::memcpy(data.getLinePointer(int(y)), picture.bgra.data() + y * row, row);
        tiles[o.display] = image;
    }
    tiles_ = std::move(tiles);
}

std::vector<juce::Image> MachinesView::tiles() const
{
    std::vector<juce::Image> images;
    if (status_)
        for (const MachineOutput& o : status_->outputs) {
            const auto it = tiles_.find(o.display);
            images.push_back(it == tiles_.end() ? juce::Image() : it->second);
        }
    return images;
}

void MachinesView::updateActions()
{
    const bool shown = status_.has_value();
    const bool safe = shown && status_->mode == "safe";
    const bool locked = shown && status_->showLock;
    for (juce::Component* c :
         std::initializer_list<juce::Component*>{&testCard_, &identify_, &unmirror_, &restart_, &safeMode_})
        c->setVisible(shown);
    lock_.setVisible(shown && !locked);
    unlock_.setVisible(locked);
    testCard_.setToggleState(shown && status_->testCard, juce::dontSendNotification);
    for (juce::Button* b : {&testCard_, &identify_, &unmirror_})
        b->setEnabled(!safe);
    restart_.setText(safe ? "Leave safe mode" : "Restart engine");
    restart_.setEnabled(!locked);
    safeMode_.setEnabled(!locked && !safe);
    const juce::String hold = "Hold " + juce::String(HoldButton::kHold, 1) + " s";
    restart_.setTooltip(locked ? "The Show lock is on" : hold + " to restart the engine through the supervisor");
    safeMode_.setTooltip(locked ? "The Show lock is on" : hold + " to restart the engine in safe mode");
    unlock_.setTooltip(hold + " to turn the Show lock off");
    const std::string mode = shown && status_->media ? status_->media->mode : "";
    for (auto [button, name] : {std::pair{&syncAuto_, "auto"}, std::pair{&syncAllow_, "allow"},
                                std::pair{&syncBlock_, "block"}}) {
        button->setVisible(!mode.empty());
        button->setToggleState(mode == name, juce::dontSendNotification);
    }

    const UpdateProgress progress = session_.client().updateProgress();
    const juce::String bundle = utf8(updateBundle_.string());
    juce::String refused;
    if (safe)
        refused = "The engine is in safe mode";
    else if (shown && !status_->updateRefused.empty())
        refused = utf8(status_->updateRefused);
    else if (!std::filesystem::exists(updateBundle_ / "Contents/MacOS/nocturne"))
        refused = "No engine bundle at " + bundle;
    listen_.setVisible(listenShown());
    volume_.setVisible(listenShown());
    listen_.setToggleState(session_.listening(), juce::dontSendNotification);
    update_.setVisible(shown);
    update_.setEnabled(shown && refused.isEmpty() && progress.state != "packing" && progress.state != "sending");
    update_.setTooltip(refused.isNotEmpty() ? refused
                                            : hold + " to install " + bundle +
                                                  " on this machine; it is rolled back if it does not run healthy");
}

void MachinesView::setUpdateBundle(std::filesystem::path app)
{
    updateBundle_ = std::move(app);
    updateActions();
}

void MachinesView::startUpdate()
{
    updateBefore_ = status_ ? status_->lastUpdate : std::nullopt;
    session_.client().sendUpdate(updateBundle_);
    updateActions();
    repaint();
}

std::pair<juce::String, juce::Colour> MachinesView::updateNote() const
{
    const Palette& p = Look::palette();
    const UpdateProgress u = session_.client().updateProgress();
    if (u.state == "packing")
        return {"Update: packing " + utf8(updateBundle_.filename().string()), p.accent};
    if (u.state == "sending")
        return {"Update: sending build " + utf8(u.build) + kDot + juce::String(u.size ? 100 * u.sent / u.size : 0) + "%",
                p.accent};
    if (u.state == "failed")
        return {"Update failed: " + utf8(u.error), p.fault};
    if (u.state != "installing")
        return {};
    const std::optional<MachineStatus::Update> last = status_ ? status_->lastUpdate : std::nullopt;
    if (!last || last == updateBefore_)
        return {"Update: installing build " + utf8(u.build) + "; the engine restarts", p.accent};
    if (last->result == "updated")
        return {"Updated to build " + utf8(last->build), p.live};
    return {"Update " + utf8(last->result) + (last->reason.empty() ? "" : ": " + utf8(last->reason)),
            last->result == "installed" ? p.accent : p.fault};
}

juce::String MachinesView::updateLine() const
{
    return updateNote().first;
}

juce::TextButton& MachinesView::syncButton(const std::string& mode)
{
    return mode == "allow" ? syncAllow_ : mode == "block" ? syncBlock_ : syncAuto_;
}

juce::Rectangle<int> MachinesView::mediaPanel() const
{
    return {kMargin, actionsPanel().getBottom() + kMargin, kSidePanel,
            kHeader + 3 + 3 * kRow + kButton + kGap + 3};
}

juce::Rectangle<int> MachinesView::actionsPanel() const
{
    return {kMargin, machinePanel().getBottom() + kMargin, kSidePanel,
            kHeader + 3 + kActionRows * (kButton + kGap) + kRow + 3};
}

void MachinesView::resized()
{
    auto area = actionsPanel().reduced(8, 0).withTrimmedTop(kHeader + 3);
    auto row = [&] {
        auto r = area.removeFromTop(kButton);
        area.removeFromTop(kGap);
        return r;
    };
    auto first = row();
    const int third = (first.getWidth() - 2 * kGap) / 3;
    testCard_.setBounds(first.removeFromLeft(third));
    unmirror_.setBounds(first.removeFromRight(third));
    identify_.setBounds(first.reduced(kGap, 0));
    auto second = row();
    restart_.setBounds(second.removeFromLeft((second.getWidth() - kGap) / 2));
    safeMode_.setBounds(second.withTrimmedLeft(kGap));
    auto bottom = row();
    update_.setBounds(bottom.removeFromRight((bottom.getWidth() - kGap) / 2));
    bottom.removeFromRight(kGap);
    lock_.setBounds(bottom);
    unlock_.setBounds(bottom);
    auto sync = mediaPanel().reduced(8, 0).removeFromBottom(kButton + kGap).withTrimmedBottom(kGap);
    const int width = (sync.getWidth() - 2 * kGap) / 3;
    syncAuto_.setBounds(sync.removeFromLeft(width));
    syncBlock_.setBounds(sync.removeFromRight(width));
    syncAllow_.setBounds(sync.reduced(kGap, 0));
    auto listen = audioPanel().reduced(8, 0).removeFromBottom(kRow + 3).withTrimmedBottom(3).withTrimmedLeft(kLabel);
    listen_.setBounds(listen.removeFromLeft(kChip));
    volume_.setBounds(listen.withTrimmedLeft(kGap));
}

bool MachinesView::listenShown() const
{
    return status_.has_value() && !session_.client().local();
}

juce::Rectangle<int> MachinesView::machinePanel() const
{
    return {kMargin, kMargin, kSidePanel, kHeader + 5 * kRow + 6};
}

juce::Rectangle<int> MachinesView::outputsPanel() const
{
    const int rows = status_ ? outputRowsHeight(*status_) : kRow;
    const int width = std::clamp(getWidth() - kSidePanel - kAudioPanel - 4 * kMargin, kSidePanel, kOutputsPanel);
    const int outputs = status_ ? int(status_->outputs.size()) : 0;
    const int tiles = outputs ? tileWidth(width - 16, outputs) * 9 / 16 + kRow + kGap : 0;
    return {machinePanel().getRight() + kMargin, kMargin, width, kHeader + rows + 6 + tiles};
}

juce::Rectangle<int> MachinesView::tilesArea() const
{
    const int rows = status_ ? outputRowsHeight(*status_) : kRow;
    return outputsPanel().withTrimmedTop(kHeader + 3 + rows).reduced(8, 0).withTrimmedBottom(3);
}

juce::Rectangle<int> MachinesView::audioPanel() const
{
    const int rows = 4 + (status_ ? djRows(*status_) : 0) + (listenShown() ? 1 : 0);
    return {outputsPanel().getRight() + kMargin, kMargin, kAudioPanel, kHeader + rows * kRow + 6};
}

int MachinesView::outputRowsHeight(const MachineStatus& status) const
{
    int height = 0;
    for (const MachineOutput& o : status.outputs)
        height += outputHeight(o, expanded_);
    return std::max(height, kRow);
}

void MachinesView::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.background);
    if (!status_) {
        g.setColour(p.muted);
        g.setFont(Look::font());
        const juce::String where = utf8(session_.host());
        drawText(g, session_.client().connected() ? "Waiting for the machine status of " + where : "Not connected to " + where,
                 getLocalBounds().reduced(kMargin * 2).removeFromTop(kRow), juce::Justification::centredLeft);
        return;
    }
    paintMachine(g, *status_);
    paintActions(g, *status_);
    paintOutputs(g, *status_);
    paintAudio(g, *status_);
    if (status_->media)
        paintMedia(g, *status_->media);
}

void MachinesView::paintMedia(juce::Graphics& g, const MachineStatus::Media& m) const
{
    const Palette& p = Look::palette();
    auto area = mediaPanel();
    paintPanel(g, area, "Media");
    area.removeFromTop(kHeader + 3);
    paintRow(g, area.removeFromTop(kRow), "Files", readinessText(m), m.ready ? p.live : m.alert.empty() ? p.accent : p.fault);
    paintRow(g, area.removeFromTop(kRow), "Sync", syncText(m), m.state == "error" ? p.fault : p.text);
    if (!m.alert.empty())
        paintRow(g, area.removeFromTop(kRow), "Alert", utf8(m.alert), p.fault);
}

void MachinesView::paintActions(juce::Graphics& g, const MachineStatus& s) const
{
    const Palette& p = Look::palette();
    auto area = actionsPanel();
    paintPanel(g, area, "Actions");
    const auto note = area.removeFromBottom(kRow + 3).withTrimmedLeft(8).withTrimmedRight(8);
    g.setFont(Look::font());
    const auto [update, updateColour] = updateNote();
    if (s.mode == "safe") {
        g.setColour(p.fault);
        drawText(g, "Safe mode: " + utf8(s.safeReason), note, juce::Justification::centredLeft, true);
    } else if (update.isNotEmpty()) {
        g.setColour(updateColour);
        drawText(g, update, note, juce::Justification::centredLeft, true);
    } else if (s.showLock) {
        g.setColour(p.accent);
        drawText(g, "Show lock on: Restart, Safe mode and Update are refused", note, juce::Justification::centredLeft,
                 true);
    } else {
        g.setColour(p.muted);
        drawText(g, "Restart, Safe mode and Update act only when held", note, juce::Justification::centredLeft, true);
    }
}

void MachinesView::paintMachine(juce::Graphics& g, const MachineStatus& s) const
{
    const Palette& p = Look::palette();
    auto area = machinePanel();
    const std::optional<VenueEntry> venue = session_.currentVenue();
    paintPanel(g, area, "Machine" + kDot + utf8(venue ? venue->venue.name : session_.host()));
    area.removeFromTop(kHeader + 3);
    paintRow(g, area.removeFromTop(kRow), "Render", orDash(s.fps, 1, " fps") + kDot + "GPU " + orDash(s.gpuMs, 2, " ms"),
             p.text);
    paintRow(g, area.removeFromTop(kRow), "Frame guard",
             s.guard ? juce::String(int64_t(s.guard)) + (s.guard == 1 ? " layer" : " layers") + " half-res"
                     : juce::String("off"),
             s.guard ? p.accent : p.text);
    const bool limited = s.cpuSpeedLimit && *s.cpuSpeedLimit < 100.0;
    juce::String cpu = "speed " + orDash(s.cpuSpeedLimit, 0, "%") + kDot +
                       orDash(s.temperatureC, 0, juce::String::fromUTF8(" \xc2\xb0" "C"));
    if (!s.thermalState.empty())
        cpu << kDot << utf8(s.thermalState);
    const bool hot = s.thermalState == "serious" || s.thermalState == "critical";
    paintRow(g, area.removeFromTop(kRow), "CPU", cpu, hot ? p.fault : limited ? p.accent : p.text);
    paintRow(g, area.removeFromTop(kRow), "Version", utf8(s.version), p.text);
    paintRow(g, area.removeFromTop(kRow), "Next Cue",
             s.nextCue ? utf8(s.nextCue->name) + kDot + "bar " + juce::String(s.nextCue->bar) + kDot +
                             inTime(s.nextCue->seconds)
                       : juce::String("none scheduled"),
             s.nextCue ? p.text : p.muted);
}

void MachinesView::paintOutputs(juce::Graphics& g, const MachineStatus& s) const
{
    const Palette& p = Look::palette();
    auto area = outputsPanel();
    paintPanel(g, area, "Outputs");
    area.removeFromTop(kHeader + 3);
    g.setFont(Look::font());
    if (s.outputs.empty()) {
        paintRow(g, area.removeFromTop(kRow), "", "No screen is bound to a display", p.muted);
        return;
    }
    for (const MachineOutput& o : s.outputs) {
        auto block = area.removeFromTop(outputHeight(o, expanded_));
        auto row = block.removeFromTop(kRow).reduced(8, 2);
        const juce::Colour colour = stateColour(o.state);
        g.setFont(Look::smallFont());
        fillWithText(g, row.removeFromLeft(kChip), colour.withAlpha(0.85f), p.liveInk, utf8(o.state),
                     juce::Justification::centred, 0);
        row.removeFromLeft(8);
        g.setFont(Look::font());
        g.setColour(p.white);
        drawText(g, utf8(o.screenName), row.removeFromLeft(kScreenName), juce::Justification::centredLeft);
        g.setColour(o.state == "ok" ? p.text : colour);
        drawText(g, outputFigures(o), row, juce::Justification::centredLeft);
        if (!expanded_.contains(o.display))
            continue;
        block.removeFromLeft(8 + kChip + 8);
        block.removeFromRight(8);
        g.setColour(p.muted);
        drawText(g,
                 "display " + utf8(o.display) + kDot + orDash(o.hz, 2, " Hz") + kDot + orDash(o.presents, 2, " presents/s") +
                     kDot + orDash(o.hitches, 0, " hitches/s"),
                 block.removeFromTop(kRow), juce::Justification::centredLeft);
        const auto history = history_.find(o.display);
        paintHistory(g, block.removeFromTop(kDetails - kRow - 4),
                     history == history_.end() ? std::deque<double>{} : history->second, o.hz.value_or(60.0));
    }
    paintTiles(g, s);
}

void MachinesView::paintTiles(juce::Graphics& g, const MachineStatus& s) const
{
    const Palette& p = Look::palette();
    auto area = tilesArea();
    const int width = tileWidth(area.getWidth(), int(s.outputs.size()));
    for (const MachineOutput& o : s.outputs) {
        auto tile = area.removeFromLeft(width).withHeight(width * 9 / 16 + kRow);
        area.removeFromLeft(kGap);
        auto caption = tile.removeFromBottom(kRow).reduced(0, 2);
        g.setFont(Look::smallFont());
        fillWithText(g, caption.removeFromLeft(kChip), stateColour(o.state).withAlpha(0.85f), p.liveInk, utf8(o.state),
                     juce::Justification::centred, 0);
        caption.removeFromLeft(6);
        g.setColour(p.text);
        drawText(g, utf8(o.screenName), caption, juce::Justification::centredLeft);
        g.setColour(juce::Colours::black);
        g.fillRect(tile);
        if (const auto it = tiles_.find(o.display); it != tiles_.end()) {
            g.drawImage(it->second, tile.toFloat(), juce::RectanglePlacement::centred);
        } else {
            g.setColour(p.muted);
            drawText(g, "no picture yet", tile, juce::Justification::centred);
        }
    }
}

void MachinesView::paintAudio(juce::Graphics& g, const MachineStatus& s) const
{
    const Palette& p = Look::palette();
    auto area = audioPanel();
    paintPanel(g, area, "Audio");
    area.removeFromTop(kHeader + 3);
    if (!s.audio || s.audio->device.empty()) {
        paintRow(g, area.removeFromTop(kRow), "Input", "no input", p.fault);
    } else {
        juce::String input = utf8(s.audio->device);
        if (!s.audio->channels.empty()) {
            juce::StringArray channels;
            for (int c : s.audio->channels)
                channels.add(juce::String(c));
            input << kDot << "ch " << channels.joinIntoString(",");
        }
        paintRow(g, area.removeFromTop(kRow), "Input", input, s.audio->state == "running" ? p.text : p.accent);
    }
    const double gain = s.audio ? s.audio->gainDb : 0.0;
    paintRow(g, area.removeFromTop(kRow), "Gain", s.audio ? (gain >= 0 ? "+" : "") + juce::String(gain, 1) + " dB" : "-",
             p.text);
    auto level = area.removeFromTop(kRow);
    paintRow(g, level, "Signal", !s.audio ? "-" : s.audio->live ? "" : "no audio", s.audio ? p.fault : p.text);
    if (s.audio && s.audio->live)
        drawBar(g, level.withTrimmedLeft(8 + kLabel).withTrimmedRight(8).reduced(0, 5), float(s.audio->level), p.meter);
    paintRow(g, area.removeFromTop(kRow), "Clock",
             juce::String(s.bpm, 1) + " BPM" + kDot + "bar " + juce::String(s.bar) + "." + juce::String(s.beatInBar),
             p.text);
    if (djRows(s)) {
        paintRow(g, area.removeFromTop(kRow), "DJ link", s.djlinkFollowing ? "following the on-air player" : "on, not following",
                 s.djlinkFollowing ? p.live : p.accent);
        for (const MachineStatus::DjPlayer& d : s.djPlayers)
            paintRow(g, area.removeFromTop(kRow), djPlayerLabel(d), djPlayerFigures(d),
                     d.onAir.value_or(false) ? p.live : d.bpm ? p.text : p.muted);
    }
    if (listenShown())
        paintRow(g, area.removeFromTop(kRow), "Listen", {}, p.text);
}

void MachinesView::mouseDown(const juce::MouseEvent& e)
{
    if (!status_ || !outputsPanel().contains(e.getPosition()))
        return;
    int y = outputsPanel().getY() + kHeader + 3;
    for (size_t row = 0; row < status_->outputs.size(); ++row) {
        const int height = outputHeight(status_->outputs[row], expanded_);
        if (e.y >= y && e.y < y + kRow)
            return toggleOutput(int(row));
        y += height;
    }
}

juce::StringArray MachinesView::outputLines() const
{
    juce::StringArray lines;
    if (status_)
        for (const MachineOutput& o : status_->outputs)
            lines.add(outputLine(o));
    return lines;
}

juce::StringArray MachinesView::mediaLines() const
{
    juce::StringArray lines;
    if (!status_ || !status_->media)
        return lines;
    lines.add(readinessText(*status_->media));
    lines.add(syncText(*status_->media));
    if (!status_->media->alert.empty())
        lines.add(utf8(status_->media->alert));
    return lines;
}

juce::StringArray MachinesView::djlinkLines() const
{
    juce::StringArray lines;
    if (status_ && djRows(*status_))
        for (const MachineStatus::DjPlayer& d : status_->djPlayers)
            lines.add(djPlayerLabel(d) + kDot + djPlayerFigures(d));
    return lines;
}

void MachinesView::toggleOutput(int row)
{
    if (!status_ || row < 0 || size_t(row) >= status_->outputs.size())
        return;
    const std::string& display = status_->outputs[size_t(row)].display;
    if (!expanded_.erase(display))
        expanded_.insert(display);
    repaint();
}

bool MachinesView::expanded(int row) const
{
    return status_ && row >= 0 && size_t(row) < status_->outputs.size() &&
           expanded_.contains(status_->outputs[size_t(row)].display);
}

} // namespace azl::ui
