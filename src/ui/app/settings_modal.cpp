#include "ui/app/settings_modal.hpp"

#include "ui/app/look.hpp"

#include <algorithm>
#include <tuple>

namespace azl::ui {

namespace {

const std::pair<uint32_t, uint32_t> kSizes[] = {{0, 0}, {3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}};
constexpr int kLabelWidth = 112;
using Section = SettingsModal::Section;
constexpr Section kSections[] = {Section::Recording, Section::Audio, Section::Displays, Section::Performance,
                                 Section::Caches, Section::Midi};
constexpr int kRow = 24;
constexpr const char* kDisplaysNote =
    "Kept on this machine, not in the show: a screen of the same name in any show goes to the same display.";

juce::String gigabytes(uint64_t megabytes)
{
    return juce::String(double(megabytes) / 1024.0, 1) + " GB";
}

// The end of a path, which names the folder, when the whole does not fit.
juce::String elideStart(const juce::String& text, int width)
{
    const juce::Font font = Look::font();
    if (juce::GlyphArrangement::getStringWidthInt(font, text) <= width)
        return text;
    const juce::String dots = juce::String::fromUTF8("\xe2\x80\xa6");
    for (int start = 1; start < text.length(); ++start)
        if (juce::GlyphArrangement::getStringWidthInt(font, dots + text.substring(start)) <= width)
            return dots + text.substring(start);
    return dots;
}

juce::String number(double value)
{
    const juce::String text = juce::String(value, 2).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return text.isEmpty() ? "0" : text;
}

// The field's value in seconds; `kept` while it still shows that, rounded.
double seconds(const juce::TextEditor& field, double unit, double kept)
{
    return field.getText() == number(kept / unit) ? kept : field.getText().getDoubleValue() * unit;
}

juce::String sizeName(uint32_t width, uint32_t height)
{
    return width == 0 ? juce::String("Composition size") : juce::String(width) + "x" + juce::String(height);
}

} // namespace

SettingsModal::SettingsModal(Session& session) : session_(session), native_(session.client().local())
{
    title_.setText("Settings", juce::dontSendNotification);
    title_.setFont(Look::font(true));
    recordingTab_.setTooltip("Where and how the engine records");
    audioTab_.setTooltip("The engine machine's audio input");
    displaysTab_.setTooltip("Which of the engine machine's displays each screen goes to, kept on that machine");
    performanceTab_.setTooltip("How the engine trades sharpness for a steady frame rate");
    cachesTab_.setTooltip("How much the engine keeps decoded");
    midiTab_.setTooltip("MIDI controllers on this machine and what their controls drive");
    for (Section section : kSections)
        sectionButton(section).onClick = [this, section] { showSection(section); };
    input_.setTooltip("The engine machine's input device; the engine reopens it after a restart or a replug");
    input_.onChange = [this] {
        const int id = input_.getSelectedId();
        if (id > 0 && size_t(id) <= inputIds_.size())
            session_.client().selectAudioInput(inputIds_[size_t(id - 1)]);
    };
    renderScale_.addItem("100% (1080p)", 100);
    renderScale_.addItem("83% (900p)", 83);
    renderScale_.addItem("67% (720p)", 67);
    renderScale_.setTooltip("The size the composition renders at, scaled up once at the output; text stays sharp");
    renderScale_.onChange = [this] { session_.client().setSettings({{"renderScale", renderScale_.getSelectedId()}}); };
    profile_.addItem("Smooth", 1);
    profile_.addItem("Sharp", 2);
    profile_.setTooltip("Smooth holds the frame rate; Sharp keeps full resolution until frames drop");
    profile_.onChange = [this] {
        session_.client().setSettings({{"profile", profile_.getSelectedId() == 2 ? "sharp" : "smooth"}});
    };
    protect_.setTooltip("During a transition, lower resolution before a frame would drop");
    protect_.onClick = [this] { session_.client().setSettings({{"protectTransitions", protect_.getToggleState()}}); };
    bake_.setTooltip("Draw the passes of a chain that do not change once and reuse them, with the same picture");
    bake_.onClick = [this] { session_.client().setSettings({{"bakeStatic", bake_.getToggleState()}}); };
    frameCache_.setTooltip("The frames each playing clip holds (MB); lowering it frees them at once");
    preload_.setTooltip("The frames of clips preloaded for the next trigger (MB); lowering it frees them at once");
    previews_.setTooltip("Clip previews kept for a selection back besides the ones shown");
    for (juce::TextEditor* field : {&frameCache_, &preload_, &previews_}) {
        field->setInputRestrictions(6, "0123456789");
        field->setFont(Look::font());
        field->setJustification(juce::Justification::centredRight);
        field->onReturnKey = [this] { sendCaches(); };
        field->onFocusLost = [this] { sendCaches(); };
        field->onEscapeKey = [this] { refreshSettings(); };
    }
    codec_.addItem("HEVC", 1);
    codec_.addItem("ProRes 422", 2);
    codec_.setTooltip("HEVC uses the hardware encoder where the engine machine has one; ProRes is larger and edits faster");
    codec_.onChange = [this] { send({{"codec", codec_.getSelectedId() == 2 ? "prores" : "hevc"}}); };
    size_.setTooltip("The recording's size; Composition size records it as rendered");
    size_.onChange = [this] {
        const size_t at = size_t(size_.getSelectedId() - 1);
        if (at < std::size(kSizes))
            send({{"width", kSizes[at].first}, {"height", kSizes[at].second}});
    };
    audio_.setTooltip("An audio track of the engine's audio input, when one runs");
    audio_.onClick = [this] { send({{"audio", audio_.getToggleState()}}); };
    precise_.setTooltip("HEVC Main10 and ProRes from 16 bits instead of 8-bit input; costs GPU time");
    precise_.onClick = [this] { send({{"precise", precise_.getToggleState()}}); };
    reserveMB_.setTooltip("A recording stops, and none starts, below this much free disk space (MB)...");
    reservePercent_.setTooltip("...or this share of the disk, whichever is larger");
    maxMB_.setTooltip("Past this size (MB) the recording goes on in the next numbered file; 0: never");
    maxSeconds_.setTooltip("Past this length (seconds) the recording goes on in the next numbered file; 0: never");
    maxTotal_.setTooltip("Past this length (hours) in all its files the recording finishes; 0: never");
    gigGrace_.setTooltip("A recording started during a gig finishes this long (minutes) after the gig's end");
    for (juce::TextEditor* field : {&reserveMB_, &reservePercent_, &maxMB_, &maxSeconds_, &maxTotal_, &gigGrace_}) {
        field->setInputRestrictions(9, "0123456789.");
        field->setFont(Look::font());
        field->setJustification(juce::Justification::centredRight);
        field->onReturnKey = [this] { sendNumbers(); };
        field->onFocusLost = [this] { sendNumbers(); };
        field->onEscapeKey = [this] { refresh(); };
    }
    choose_.setTooltip("Choose the folder on the engine machine");
    choose_.onClick = [this] { choose(); };
    close_.onClick = [this] { setVisible(false); };
    list_.setModel(this);
    list_.setRowHeight(24);
    list_.setColour(juce::ListBox::backgroundColourId, Look::palette().field);
    list_.setTooltip("Click a volume or folder to open it");
    up_.setTooltip("The folder above");
    up_.onClick = [this] {
        if (!listed_.parent.empty())
            browse(listed_.parent);
    };
    use_.getProperties().set("accent", true);
    use_.setTooltip("Record into the folder shown");
    use_.onClick = [this] {
        if (!listed_.path.empty())
            pick(listed_.path);
    };
    cancel_.onClick = [this] { showPicker(false); };
    addAndMakeVisible(title_);
    addAndMakeVisible(close_);
    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &recordingTab_, &audioTab_, &displaysTab_, &performanceTab_, &cachesTab_, &choose_, &codec_, &size_,
             &audio_, &precise_, &reserveMB_, &reservePercent_, &maxMB_, &maxSeconds_, &maxTotal_, &gigGrace_, &list_,
             &up_, &use_, &cancel_, &input_, &renderScale_, &profile_, &protect_, &bake_, &frameCache_, &preload_, &previews_, &midiTab_, &midi_})
        addChildComponent(c);
    session_.addView(this);
    refresh();
    refreshAudio();
    refreshDisplays();
    refreshSettings();
    showSection(Section::Recording);
}

SettingsModal::~SettingsModal()
{
    session_.removeView(this);
}

void SettingsModal::sessionChanged(int changes)
{
    if (changes & (Session::Record | Session::Connection))
        refresh();
    if (changes & (Session::Settings | Session::Connection))
        refreshSettings();
    if (changes & (Session::Document | Session::Outputs | Session::Connection))
        refreshDisplays();
    if (changes & (Session::Audio | Session::Connection)) {
        const AudioInputState in = session_.client().audioInput();
        juce::String key = utf8(in.requested + "|" + in.uid);
        for (const AudioDevice& d : session_.client().audioDevices())
            key << "|" << utf8(d.uid) << ":" << d.channels;
        if (key != audioKey_ || (changes & Session::Connection)) {
            audioKey_ = key;
            refreshAudio();
        }
        if (section_ == Section::Audio)
            repaint();
    }
}

void SettingsModal::refresh()
{
    const std::string folder = record_.folder;
    record_ = session_.client().record();
    if (picking_) {
        listed_ = session_.client().folders();
        list_.updateContent();
        list_.repaint();
    } else if (record_.folder != folder && record_.folderProblem.empty() && !record_.folder.empty()) {
        listed_ = {};
        session_.client().requestFolders(record_.folder);
    } else {
        listed_ = session_.client().folders();
    }
    codec_.setSelectedId(record_.codec == "prores" ? 2 : 1, juce::dontSendNotification);
    size_.clear(juce::dontSendNotification);
    int id = 0;
    for (const auto& [w, h] : kSizes) {
        size_.addItem(sizeName(w, h), ++id);
        if (w == record_.width && h == record_.height)
            size_.setSelectedId(id, juce::dontSendNotification);
    }
    if (size_.getSelectedId() == 0) {
        size_.addItem(sizeName(record_.width, record_.height), ++id);
        size_.setSelectedId(id, juce::dontSendNotification);
    }
    audio_.setToggleState(record_.audio != "off", juce::dontSendNotification);
    precise_.setToggleState(record_.precise, juce::dontSendNotification);
    const std::pair<juce::TextEditor*, juce::String> numbers[] = {
        {&reserveMB_, juce::String(int64_t(record_.reserveMB))},
        {&reservePercent_, juce::String(record_.reservePercent, 1).trimCharactersAtEnd("0").trimCharactersAtEnd(".")},
        {&maxMB_, juce::String(int64_t(record_.maxMB))},
        {&maxSeconds_, juce::String(record_.maxSeconds, 1).trimCharactersAtEnd("0").trimCharactersAtEnd(".")},
        {&maxTotal_, number(record_.maxTotalSeconds / 3600.0)},
        {&gigGrace_, number(record_.gigGraceSeconds / 60.0)}};
    for (const auto& [field, text] : numbers)
        if (!field->hasKeyboardFocus(false))
            field->setText(text.isEmpty() ? "0" : text, false);
    const bool connected = session_.client().connected();
    for (juce::Component* c : std::initializer_list<juce::Component*>{&choose_, &codec_, &size_, &audio_, &precise_})
        c->setEnabled(connected && !record_.recording);
    repaint();
}

juce::String SettingsModal::freeText() const
{
    if (!record_.folderProblem.empty() || picking_ || listed_.path.empty())
        return {};
    return gigabytes(listed_.freeMB) + " free";
}

juce::String SettingsModal::folderText() const
{
    if (!record_.folderProblem.empty())
        return utf8(record_.folderProblem);
    const juce::String free = freeText();
    return utf8(record_.folder) + (free.isEmpty() ? juce::String() : juce::String::fromUTF8("  \xc2\xb7  ") + free);
}

void SettingsModal::send(const Message& fields)
{
    session_.client().setRecordSettings(fields);
}

void SettingsModal::sendNumbers()
{
    const Message fields{{"reserveMB", uint64_t(reserveMB_.getText().getLargeIntValue())},
                         {"reservePercent", reservePercent_.getText().getDoubleValue()},
                         {"maxMB", uint64_t(maxMB_.getText().getLargeIntValue())},
                         {"maxSeconds", maxSeconds_.getText().getDoubleValue()},
                         {"maxTotalSeconds", seconds(maxTotal_, 3600.0, record_.maxTotalSeconds)},
                         {"gigGraceSeconds", seconds(gigGrace_, 60.0, record_.gigGraceSeconds)}};
    if (fields["reserveMB"] != record_.reserveMB || fields["reservePercent"] != record_.reservePercent ||
        fields["maxMB"] != record_.maxMB || fields["maxSeconds"] != record_.maxSeconds ||
        fields["maxTotalSeconds"] != record_.maxTotalSeconds || fields["gigGraceSeconds"] != record_.gigGraceSeconds)
        send(fields);
}

void SettingsModal::choose()
{
    if (!native_)
        return browse(record_.folderProblem.empty() ? record_.folder : std::string());
    chooser_ = std::make_unique<juce::FileChooser>("Record into", juce::File(utf8(record_.folder)));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& chooser) {
                              if (chooser.getResult() != juce::File())
                                  pick(chooser.getResult().getFullPathName().toStdString());
                          });
}

void SettingsModal::browse(std::string path)
{
    showPicker(true);
    session_.client().requestFolders(path);
}

const EngineFolder* SettingsModal::row(int index) const
{
    if (index < 0)
        return nullptr;
    if (size_t(index) < listed_.volumes.size())
        return &listed_.volumes[size_t(index)];
    const size_t folder = size_t(index) - listed_.volumes.size();
    return folder < listed_.folders.size() ? &listed_.folders[folder] : nullptr;
}

void SettingsModal::enter(int index)
{
    if (const EngineFolder* folder = row(index))
        browse(folder->path);
}

void SettingsModal::pick(std::string folder)
{
    showPicker(false);
    send({{"path", folder}});
}

void SettingsModal::showPicker(bool on)
{
    picking_ = on;
    updateVisibility();
    if (!on && record_.folderProblem.empty() && !record_.folder.empty())
        session_.client().requestFolders(record_.folder);
    listed_ = {};
    list_.updateContent();
    repaint();
}

void SettingsModal::paintListBoxItem(int index, juce::Graphics& g, int width, int height, bool selected)
{
    const EngineFolder* folder = row(index);
    if (!folder)
        return;
    const Palette& p = Look::palette();
    const bool volume = size_t(index) < listed_.volumes.size();
    g.fillAll(selected ? p.buttonHover : p.field);
    auto area = juce::Rectangle<int>(width, height).reduced(8, 0);
    g.setFont(Look::font());
    if (volume) {
        g.setColour(p.muted);
        drawText(g, gigabytes(folder->freeMB) + " free of " + gigabytes(folder->totalMB), area,
                 juce::Justification::centredRight);
        area.removeFromRight(150);
    }
    g.setColour(volume ? p.live : p.text);
    drawText(g, utf8(folder->name), area, juce::Justification::centredLeft, true);
}

juce::TextButton& SettingsModal::sectionButton(Section section)
{
    switch (section) {
    case Section::Audio: return audioTab_;
    case Section::Displays: return displaysTab_;
    case Section::Performance: return performanceTab_;
    case Section::Caches: return cachesTab_;
    case Section::Midi: return midiTab_;
    case Section::Recording: break;
    }
    return recordingTab_;
}

bool SettingsModal::offers(Section section) const
{
    static const char* const names[] = {"recording", "audio", "displays", "performance", "caches"};
    // MIDI is this machine's, whatever the engine serves.
    return section == Section::Recording || section == Section::Midi ||
           std::find(settings_.sections.begin(), settings_.sections.end(), names[int(section)]) !=
               settings_.sections.end();
}

void SettingsModal::showSection(Section section)
{
    if (!offers(section))
        section = Section::Recording;
    if (picking_ && section != Section::Recording)
        showPicker(false);
    section_ = section;
    for (Section s : kSections)
        sectionButton(s).setToggleState(s == section_, juce::dontSendNotification);
    if (section_ == Section::Caches)
        session_.client().requestSettings();
    updateVisibility();
    resized();
    repaint();
}

std::vector<juce::Component*> SettingsModal::sectionComponents(Section section)
{
    switch (section) {
    case Section::Recording:
        return {&choose_, &codec_, &size_, &audio_, &precise_, &reserveMB_, &reservePercent_, &maxMB_, &maxSeconds_,
                &maxTotal_, &gigGrace_};
    case Section::Audio: return {&input_};
    case Section::Displays: {
        std::vector<juce::Component*> boxes;
        for (auto& box : screens_)
            boxes.push_back(box.get());
        return boxes;
    }
    case Section::Performance: return {&renderScale_, &profile_, &protect_, &bake_};
    case Section::Caches: return {&frameCache_, &preload_, &previews_};
    case Section::Midi: return {&midi_};
    }
    return {};
}

void SettingsModal::updateVisibility()
{
    for (Section s : kSections) {
        sectionButton(s).setVisible(offers(s));
        for (juce::Component* c : sectionComponents(s))
            c->setVisible(s == section_ && !(s == Section::Recording && picking_));
    }
    for (juce::Component* c : std::initializer_list<juce::Component*>{&list_, &up_, &use_, &cancel_})
        c->setVisible(section_ == Section::Recording && picking_);
}

void SettingsModal::visibilityChanged()
{
    if (isVisible()) {
        session_.client().requestSettings();
        startTimerHz(1);
    } else {
        stopTimer();
    }
}

void SettingsModal::timerCallback()
{
    if (section_ == Section::Caches)
        session_.client().requestSettings();
}

void SettingsModal::refreshSettings()
{
    settings_ = session_.client().settings();
    if (!offers(section_))
        showSection(Section::Recording);
    renderScale_.setSelectedId(int(settings_.renderScale), juce::dontSendNotification);
    profile_.setSelectedId(settings_.profile == "sharp" ? 2 : 1, juce::dontSendNotification);
    protect_.setToggleState(settings_.protectTransitions, juce::dontSendNotification);
    bake_.setToggleState(settings_.bakeStatic, juce::dontSendNotification);
    const std::pair<juce::TextEditor*, uint64_t> fields[] = {
        {&frameCache_, settings_.frameCacheMB}, {&preload_, settings_.preloadMB}, {&previews_, settings_.clipPreviews}};
    for (const auto& [field, value] : fields)
        if (!field->hasKeyboardFocus(false))
            field->setText(juce::String(int64_t(value)), false);
    updateVisibility();
}

void SettingsModal::refreshAudio()
{
    const std::vector<AudioDevice> devices = session_.client().audioDevices();
    const AudioInputState in = session_.client().audioInput();
    input_.clear(juce::dontSendNotification);
    inputIds_ = {"", "off"};
    input_.addItem("Default input", 1);
    input_.addItem("Off", 2);
    int selected = in.requested == "off" ? 2 : 1;
    for (const AudioDevice& d : devices) {
        inputIds_.push_back(d.uid);
        const int id = int(inputIds_.size());
        input_.addItem(utf8(d.name) + "  (" + juce::String(d.channels) + (d.channels == 1 ? " ch)" : " ch)"), id);
        if (!in.requested.empty() && in.requested != "off" &&
            (d.uid == in.requested || juce::String(utf8(d.name)).containsIgnoreCase(utf8(in.requested))))
            selected = id;
    }
    input_.setSelectedId(selected, juce::dontSendNotification);
    input_.setEnabled(session_.client().connected());
}

void SettingsModal::refreshDisplays()
{
    const std::shared_ptr<const Composition> composition = session_.client().composition();
    const std::vector<OutputDisplay> displays = session_.client().displays();
    const size_t count = composition->screens.size();
    if (screens_.size() != count) {
        screens_.clear();
        for (size_t i = 0; i < count; ++i) {
            auto box = std::make_unique<juce::ComboBox>();
            box->setTooltip("The engine machine's display for a screen of this name, in every show");
            box->onChange = [this, i] {
                const int id = screens_[i]->getSelectedId();
                if (id > 0 && size_t(id) <= screenDisplays_[i].size())
                    session_.client().edit(SetScreenDisplay{screenIds_[i], screenDisplays_[i][size_t(id - 1)]});
            };
            addChildComponent(*box);
            screens_.push_back(std::move(box));
        }
        resized();
    }
    const auto nameOf = [&](const std::string& id) {
        for (const OutputDisplay& d : displays)
            if (d.id == id)
                return utf8(d.name);
        return utf8(id);
    };
    screenIds_.assign(count, 0);
    screenDisplays_.assign(count, {});
    for (size_t i = 0; i < count; ++i) {
        const Screen& screen = composition->screens[i];
        juce::ComboBox& box = *screens_[i];
        box.clear(juce::dontSendNotification);
        screenIds_[i] = screen.id;
        std::vector<std::string>& ids = screenDisplays_[i];
        ids.push_back("");
        box.addItem("None", 1);
        int selected = screen.display.empty() ? 1 : 0;
        for (const OutputDisplay& d : displays) {
            ids.push_back(d.id);
            juce::String text = utf8(d.name) + "  " + juce::String(d.width) + "x" + juce::String(d.height);
            if (!d.mirrorOf.empty())
                text << "  (mirrors " << nameOf(d.mirrorOf) << ")";
            box.addItem(text, int(ids.size()));
            if (d.id == screen.display)
                selected = int(ids.size());
        }
        if (selected == 0) {
            ids.push_back(screen.display);
            box.addItem(utf8(screen.display) + "  (missing)", int(ids.size()));
            selected = int(ids.size());
        }
        box.setSelectedId(selected, juce::dontSendNotification);
        box.setEnabled(session_.client().connected());
    }
    updateVisibility();
}

juce::String SettingsModal::inputText() const
{
    const AudioInputState in = session_.client().audioInput();
    const std::vector<AudioDevice> devices = session_.client().audioDevices();
    if (in.state == "off")
        return "Off";
    if (in.uid.empty())
        return "No input device";
    int total = 0;
    for (const AudioDevice& d : devices)
        if (d.uid == in.uid)
            total = d.channels;
    juce::String text = utf8(in.name);
    if (in.channels.empty()) {
        text << "  " << juce::String::fromUTF8("\xc2\xb7") << "  all channels";
    } else {
        text << "  " << juce::String::fromUTF8("\xc2\xb7") << "  ch ";
        for (size_t i = 0; i < in.channels.size(); ++i)
            text << (i ? "+" : "") << in.channels[i];
    }
    if (total > 0)
        text << " of " << total;
    if (in.sampleRate > 0.0)
        text << "  " << juce::String::fromUTF8("\xc2\xb7") << "  " << juce::String(in.sampleRate / 1000.0, 1) << " kHz";
    if (in.state == "fallback")
        text << "  (the picked device is missing)";
    if (!in.permission.empty() && in.permission != "authorized")
        text << "  " << juce::String::fromUTF8("\xc2\xb7") << "  microphone " << utf8(in.permission);
    return text;
}

juce::String SettingsModal::useText(const std::string& field) const
{
    if (field == "frameCacheMB")
        return "MB, " + juce::String(int64_t(settings_.use.frameCacheMB)) + " MB in use";
    if (field == "preloadMB")
        return "MB, " + juce::String(int64_t(settings_.use.preloadMB)) + " MB in use";
    return "clips, " + juce::String(int64_t(settings_.use.clipPreviews)) + " kept (" +
           juce::String(int64_t(settings_.use.clipPreviewKB)) + " KB)";
}

juce::String SettingsModal::overriddenText(const std::string& field) const
{
    const auto it = settings_.overridden.find(field);
    return it == settings_.overridden.end() ? juce::String() : "set by " + utf8(it->second) + " on each start";
}

void SettingsModal::sendCaches()
{
    const Message fields{{"frameCacheMB", uint64_t(frameCache_.getText().getLargeIntValue())},
                         {"preloadMB", uint64_t(preload_.getText().getLargeIntValue())},
                         {"clipPreviews", uint64_t(previews_.getText().getLargeIntValue())}};
    if (fields["frameCacheMB"] != settings_.frameCacheMB || fields["preloadMB"] != settings_.preloadMB ||
        fields["clipPreviews"] != settings_.clipPreviews)
        session_.client().setSettings(fields);
}

juce::Rectangle<int> SettingsModal::content() const
{
    auto area = panel().reduced(12);
    area.removeFromTop(32);
    area.removeFromBottom(36);
    area.removeFromLeft(112);
    return area;
}

void SettingsModal::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(juce::Colours::black.withAlpha(0.55f));
    g.setColour(p.panel);
    g.fillRect(panel());
    g.setFont(Look::font());
    switch (section_) {
    case Section::Recording: return paintRecording(g);
    case Section::Audio: return paintAudio(g);
    case Section::Displays: return paintDisplays(g);
    case Section::Performance:
    case Section::Caches: return paintSettings(g);
    case Section::Midi: return;
    }
}

namespace {

juce::Rectangle<int> rowAt(juce::Rectangle<int> area, int index)
{
    return area.withY(area.getY() + index * (kRow + 8)).withHeight(kRow);
}

void label(juce::Graphics& g, juce::Rectangle<int> row, const juce::String& text)
{
    g.setColour(Look::palette().muted);
    drawText(g, text, row.withWidth(kLabelWidth), juce::Justification::centredLeft, true);
}

// Muted text right of a control in the row.
void note(juce::Graphics& g, juce::Rectangle<int> row, const juce::Component& control, const juce::String& text,
          juce::Colour colour)
{
    g.setColour(colour);
    drawText(g, text, row.withLeft(control.getRight() + 8), juce::Justification::centredLeft, true);
}

} // namespace

void SettingsModal::paintAudio(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const auto area = content();
    label(g, rowAt(area, 0), "Input");
    label(g, rowAt(area, 1), "In use");
    g.setColour(p.text);
    drawText(g, inputText(), rowAt(area, 1).withTrimmedLeft(kLabelWidth), juce::Justification::centredLeft, true);
    label(g, rowAt(area, 2), "Level");
    const AudioLevels levels = session_.client().audio();
    const auto level = levels.values.find("level");
    const double value = level == levels.values.end() ? 0.0 : std::clamp(level->second, 0.0, 1.0);
    auto meter = rowAt(area, 2).withTrimmedLeft(kLabelWidth).withWidth(260).reduced(0, 7);
    g.setColour(p.field);
    g.fillRect(meter);
    g.setColour(levels.live ? p.live : p.muted);
    g.fillRect(meter.withWidth(int(meter.getWidth() * value)));
    if (const juce::String flag = overriddenText("audioInput"); flag.isNotEmpty())
        note(g, rowAt(area, 0), input_, flag, p.accent);
}

void SettingsModal::paintDisplays(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const auto area = content();
    const std::shared_ptr<const Composition> composition = session_.client().composition();
    for (size_t i = 0; i < screens_.size() && i < composition->screens.size(); ++i)
        label(g, rowAt(area, int(i)), utf8(composition->screens[i].name));
    g.setColour(p.muted);
    if (screens_.empty()) {
        drawText(g, "The show has no screens", rowAt(area, 0), juce::Justification::centredLeft);
        return;
    }
    drawText(g, kDisplaysNote, rowAt(area, int(screens_.size()) + 1), juce::Justification::centredLeft, true);
}

void SettingsModal::paintSettings(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    const auto area = content();
    if (section_ == Section::Performance) {
        label(g, rowAt(area, 0), "Render scale");
        note(g, rowAt(area, 0), renderScale_, "Text layers and those above them stay at output size", p.muted);
        label(g, rowAt(area, 1), "Profile");
        note(g, rowAt(area, 1), profile_, overriddenText("profile"), p.accent);
        note(g, rowAt(area, 2), protect_, overriddenText("protectTransitions"), p.accent);
        note(g, rowAt(area, 3), bake_, overriddenText("bakeStatic"), p.accent);
        g.setColour(p.muted);
        drawText(g, "Smooth holds the frame rate: layers drop to half resolution as the GPU nears its budget.",
                 rowAt(area, 4), juce::Justification::centredLeft, true);
        drawText(g, "Sharp keeps full resolution until frames actually drop.", rowAt(area, 5),
                 juce::Justification::centredLeft, true);
        drawText(g, "Protect transitions lowers resolution during a transition before a frame would drop.",
                 rowAt(area, 6), juce::Justification::centredLeft, true);
        drawText(g, "Bake static passes draws what does not change in a chain once, with the same picture.",
                 rowAt(area, 7), juce::Justification::centredLeft, true);
        return;
    }
    const std::tuple<int, const char*, const juce::TextEditor*, const char*> rows[] = {
        {0, "Frame cache", &frameCache_, "frameCacheMB"},
        {1, "Preload", &preload_, "preloadMB"},
        {2, "Kept previews", &previews_, "clipPreviews"}};
    for (const auto& [index, name, field, key] : rows) {
        const auto row = rowAt(area, index);
        label(g, row, name);
        const juce::String flag = overriddenText(key);
        note(g, row, *field, useText(key) + (flag.isEmpty() ? juce::String() : "  " + flag),
             flag.isEmpty() ? p.muted : p.accent);
    }
}

juce::Rectangle<int> SettingsModal::panel() const
{
    return getLocalBounds().withSizeKeepingCentre(std::min(getWidth() - 40, 640), std::min(getHeight() - 40, 364));
}

void SettingsModal::paintRecording(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setColour(p.muted);
    const auto label = [&](juce::Rectangle<int> row, const char* text) {
        drawText(g, text, row.withWidth(kLabelWidth), juce::Justification::centredLeft);
    };
    if (picking_) {
        g.setColour(p.text);
        juce::String where = utf8(listed_.path.empty() ? std::string("Listing...") : listed_.path);
        if (!listed_.path.empty())
            where << juce::String::fromUTF8("  \xc2\xb7  ") << gigabytes(listed_.freeMB) << " free"
                  << (listed_.writable ? "" : ", not writable");
        drawText(g, where, pathRow_, juce::Justification::centredLeft, true);
        return;
    }
    label(folderRow_, "Folder");
    label(codecRow_, "Codec");
    label(sizeRow_, "Size");
    label(reserveRow_, "Keep free");
    label(splitRow_, "New file past");
    label(finishRow_, "Finish after");
    const auto unit = [&](const juce::TextEditor& field, const char* text) {
        drawText(g, text, field.getBounds().withX(field.getRight() + 4).withWidth(80), juce::Justification::centredLeft);
    };
    unit(reserveMB_, "MB or");
    unit(reservePercent_, "% of the disk");
    unit(maxMB_, "MB or");
    unit(maxSeconds_, "s (0: never)");
    unit(maxTotal_, "h or");
    drawText(g, "min after the gig", gigGrace_.getBounds().withX(gigGrace_.getRight() + 4).withWidth(140),
             juce::Justification::centredLeft);
    auto folder = folderRow_.withTrimmedLeft(kLabelWidth).withTrimmedRight(choose_.getWidth() + 8);
    if (!record_.folderProblem.empty()) {
        g.setColour(p.fault);
        return drawText(g, folderText(), folder, juce::Justification::centredLeft, true);
    }
    g.setColour(p.muted);
    drawText(g, freeText(), folder.removeFromRight(90), juce::Justification::centredRight);
    g.setColour(p.text);
    drawText(g, elideStart(utf8(record_.folder), folder.getWidth() - 8), folder, juce::Justification::centredLeft);
}

void SettingsModal::resized()
{
    auto area = panel().reduced(12);
    title_.setBounds(area.removeFromTop(24));
    area.removeFromTop(8);
    auto buttons = area.removeFromBottom(28);
    close_.setBounds(buttons.removeFromRight(80));
    area.removeFromBottom(8);
    auto tabs = area.removeFromLeft(100);
    for (Section section : kSections) {
        if (!offers(section))
            continue;
        sectionButton(section).setBounds(tabs.removeFromTop(kRow));
        tabs.removeFromTop(4);
    }
    area.removeFromLeft(12);
    const auto at = [&](int index) { return area.withY(area.getY() + index * (kRow + 8)).withHeight(kRow); };
    input_.setBounds(at(0).withTrimmedLeft(kLabelWidth).withWidth(260));
    for (size_t i = 0; i < screens_.size(); ++i)
        screens_[i]->setBounds(at(int(i)).withTrimmedLeft(kLabelWidth).withWidth(300));
    renderScale_.setBounds(at(0).withTrimmedLeft(kLabelWidth).withWidth(120));
    profile_.setBounds(at(1).withTrimmedLeft(kLabelWidth).withWidth(120));
    protect_.setBounds(at(2).withTrimmedLeft(kLabelWidth).withWidth(160).withSizeKeepingCentre(160, 16));
    bake_.setBounds(at(3).withTrimmedLeft(kLabelWidth).withWidth(160).withSizeKeepingCentre(160, 16));
    frameCache_.setBounds(at(0).withTrimmedLeft(kLabelWidth).withWidth(70));
    preload_.setBounds(at(1).withTrimmedLeft(kLabelWidth).withWidth(70));
    previews_.setBounds(at(2).withTrimmedLeft(kLabelWidth).withWidth(70));
    midi_.setBounds(area);

    pathRow_ = area.removeFromTop(kRow);
    up_.setBounds(pathRow_.removeFromRight(48));
    auto pickerButtons = area.removeFromBottom(kRow);
    use_.setBounds(pickerButtons.removeFromRight(120));
    pickerButtons.removeFromRight(6);
    cancel_.setBounds(pickerButtons.removeFromRight(80));
    list_.setBounds(area.withTrimmedTop(6).withTrimmedBottom(6));

    auto rows = area.withTop(pathRow_.getY());
    const auto next = [&] {
        auto r = rows.removeFromTop(kRow);
        rows.removeFromTop(8);
        return r;
    };
    folderRow_ = next();
    choose_.setBounds(folderRow_.withLeft(folderRow_.getRight() - 84));
    codecRow_ = next();
    codec_.setBounds(codecRow_.withTrimmedLeft(kLabelWidth).withWidth(160));
    sizeRow_ = next();
    size_.setBounds(sizeRow_.withTrimmedLeft(kLabelWidth).withWidth(160));
    auto toggles = next().withTrimmedLeft(kLabelWidth);
    toggles = toggles.withSizeKeepingCentre(toggles.getWidth(), 16);
    audio_.setBounds(toggles.removeFromLeft(110));
    precise_.setBounds(toggles.removeFromLeft(150));
    reserveRow_ = next();
    reserveMB_.setBounds(reserveRow_.withTrimmedLeft(kLabelWidth).withWidth(70));
    reservePercent_.setBounds(reserveMB_.getBounds().translated(70 + 50, 0).withWidth(44));
    splitRow_ = next();
    maxMB_.setBounds(splitRow_.withTrimmedLeft(kLabelWidth).withWidth(70));
    maxSeconds_.setBounds(maxMB_.getBounds().translated(70 + 50, 0).withWidth(60));
    finishRow_ = next();
    maxTotal_.setBounds(finishRow_.withTrimmedLeft(kLabelWidth).withWidth(70));
    gigGrace_.setBounds(maxTotal_.getBounds().translated(70 + 50, 0).withWidth(44));
}

} // namespace azl::ui
