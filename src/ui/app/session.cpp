#include "ui/app/session.hpp"

#include "ui/app/midi.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace azl::ui {

namespace {

constexpr double kStatusSeconds = 6.0;
constexpr double kFocusSeconds = 1.0;
// A local engine the app just spawned takes a few seconds to its first frame; show "connecting…" without the
// errno until this passes, so a freshly started engine never flashes a connection error.
constexpr double kConnectGraceSeconds = 8.0;

juce::Image imageOf(const PreviewPicture& picture)
{
    if (!picture.jpeg.empty())
        return juce::ImageFileFormat::loadFrom(picture.jpeg.data(), picture.jpeg.size());
    if (picture.bgra.empty())
        return {};
    juce::Image image(juce::Image::ARGB, int(picture.width), int(picture.height), false);
    juce::Image::BitmapData data(image, juce::Image::BitmapData::writeOnly);
    const size_t row = size_t(picture.width) * 4;
    for (uint32_t y = 0; y < picture.height; ++y)
        std::memcpy(data.getLinePointer(int(y)), picture.bgra.data() + y * row, row);
    return image;
}

double now()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

std::tuple<uint64_t, uint64_t, std::string> keyOf(const ParamRef& ref)
{
    return {ref.layer, ref.effect, ref.param};
}

// The media of the clips the layer and the layers in it play, by the document and by the clock state.
void addShownMedia(const Layer& layer, const ClockState& clock, std::set<std::string>& media)
{
    auto addCell = [&](uint64_t column) {
        auto clip = layer.clips.find(column);
        if (clip != layer.clips.end() && !clip->second.hash.empty())
            media.insert(clip->second.hash);
    };
    if (layer.playing)
        addCell(layer.playing->column);
    if (auto shown = clock.clips.find(layer.id); shown != clock.clips.end())
        addCell(shown->second.column);
    for (const Layer& inner : layer.layers)
        addShownMedia(inner, clock, media);
}

std::set<std::string> compositionMedia(const Composition& composition)
{
    std::set<std::string> media;
    forEachLayer(composition.layers, [&](const Layer& layer) {
        for (const auto& [column, clip] : layer.clips)
            if (!clip.hash.empty())
                media.insert(clip.hash);
    });
    return media;
}

DirectorMedia::Options withLibrary(DirectorMedia::Options media, const LocalPreviews::Options& local, bool rendersLocally)
{
    if (media.library.empty())
        media.library = !local.library.empty() ? local.library : rendersLocally ? MediaLibrary::defaultRoot() : "";
    return media;
}

std::optional<std::pair<uint64_t, uint64_t>> cellOf(const std::set<std::string>& hashes,
                                                    const std::optional<Session::ClipCell>& cell)
{
    if (hashes.empty() || !cell)
        return std::nullopt;
    return std::pair{cell->layer, cell->column};
}

} // namespace

Session::Session(std::string host, uint16_t port, bool surfaces, LocalRender localRender,
                 LocalPreviews::Options localOptions, DirectorMedia::Options media)
    : host_(std::move(host)), port_(port), surfaces_(surfaces), localRender_(localRender), localOptions_(localOptions),
      client_(*this)
{
    const bool rendersLocally =
        localRender_ == LocalRender::On || (localRender_ == LocalRender::Auto && !isLocalHost(host_));
    media_ = std::make_unique<DirectorMedia>(withLibrary(std::move(media), localOptions_, rendersLocally),
                                             [this](const std::string& hash) { mediaChanged(hash); });
    client_.setListenSink([this](const Message& message) { returnAudio_.receive(message); });
    midi_ = std::make_unique<MidiControl>(*this);
    connect();
    startTimerHz(30);
}

Session::~Session()
{
    midi_.reset();
    stopTimer();
    venues_.reset();
    local_.reset();
    connection_.reset();
    media_.reset();
    cancelPendingUpdate();
}

void Session::setListening(bool on)
{
    client_.listen(on);
    if (!on)
        returnAudio_.stop();
}

void Session::addView(View* view)
{
    views_.push_back(view);
}

void Session::removeView(View* view)
{
    std::erase(views_, view);
}

const Layer* Session::layer(uint64_t id) const
{
    const Layer* found = nullptr;
    forEachLayer(composition_->layers, [&](const Layer& layer) {
        if (layer.id == id)
            found = &layer;
    });
    return found;
}

const Deck* Session::shownDeck() const
{
    for (const Deck& deck : composition_->decks)
        if (deck.id == composition_->deck)
            return &deck;
    return composition_->decks.empty() ? nullptr : &composition_->decks.front();
}

juce::Image Session::still(const std::string& hash)
{
    if (auto it = stills_.find(hash); it != stills_.end())
        return it->second;
    std::optional<std::vector<uint8_t>> own = media_->still(hash);
    const std::vector<uint8_t> jpeg = own ? std::move(*own) : client_.still(hash);
    if (jpeg.empty())
        return {};
    if (!own)
        media_->storeStill(hash, jpeg);
    juce::Image image = juce::ImageFileFormat::loadFrom(jpeg.data(), jpeg.size());
    stills_[hash] = image;
    return image;
}

void Session::keepClip(const std::string& hash, std::optional<bool> keep)
{
    media_->setClip(hash, keep);
    reconcileMedia();
    notify(Media);
}

std::string Session::currentShow() const
{
    const std::string file = client_.savedPath();
    return file.empty() ? std::string() : showKey(host_, file);
}

void Session::keepShow(bool keep)
{
    const std::string show = currentShow();
    if (show.empty())
        return;
    media_->setShow(show, keep, compositionMedia(*composition_));
    reconcileMedia();
    notify(Media);
}

void Session::reconcileMedia()
{
    media_->reconcile(currentShow(), compositionMedia(*composition_));
}

// On any thread: the follower retries clips of media that arrived, and every source is split again.
void Session::mediaChanged(const std::string& hash)
{
    {
        std::lock_guard lock(mediaMutex_);
        mediaArrived_.insert(hash);
    }
    post(Media | Stills);
}

juce::Image Session::preview(const std::string& source) const
{
    auto it = previews_.find(source);
    return it == previews_.end() ? juce::Image() : it->second.image;
}

void Session::selectLayer(uint64_t layer)
{
    selectedLayer_ = layer;
    selectedClip_.reset();
    selection_.clear();
    chainTab_ = 1;
    updateWatch();
    notify(Selection);
}

void Session::selectClip(ClipCell cell)
{
    selectedLayer_ = cell.layer;
    selectedClip_ = cell;
    selection_ = {cell};
    anchor_ = cell;
    chainTab_ = 2;
    updateWatch();
    notify(Selection);
}

void Session::selectComposition()
{
    selectedLayer_.reset();
    selectedClip_.reset();
    selection_.clear();
    chainTab_ = 0;
    updateWatch();
    notify(Selection);
}

const ClipRef* Session::clip(ClipCell cell) const
{
    const Layer* owner = layer(cell.layer);
    if (!owner)
        return nullptr;
    auto it = owner->clips.find(cell.column);
    return it == owner->clips.end() ? nullptr : &it->second;
}

std::vector<Session::ClipCell> Session::clipTargets(ClipCell cell) const
{
    if (!selection_.contains(cell))
        return {cell};
    std::vector<ClipCell> cells{cell};
    for (const ClipCell& other : selection_)
        if (other != cell && clip(other))
            cells.push_back(other);
    return cells;
}

void Session::edit(std::vector<Edit> edits)
{
    if (edits.size() == 1)
        client_.edit(edits.front());
    else if (!edits.empty())
        client_.edit(Batch{std::move(edits)});
}

void Session::editClips(ClipCell cell, const ClipEdit& clipEdit)
{
    lastClipEdit_ = clipEdit;
    std::vector<Edit> edits;
    for (const ClipCell& target : clipTargets(cell))
        if (const ClipRef* c = clip(target)) {
            std::vector<Edit> more = clipEdit.edits(target, *c);
            edits.insert(edits.end(), more.begin(), more.end());
        }
    edit(std::move(edits));
}

Session::ClipEdit Session::addingEffect(const Effect& effect)
{
    Effect copy = effect;
    copy.id = 0;
    return {"add " + juce::String(copy.type), [copy](ClipCell c, const ClipRef&) {
                return std::vector<Edit>{AddEffect{c.layer, std::nullopt, copy, c.column}};
            }};
}

Session::ClipEdit Session::removingEffects(const std::string& type)
{
    return {"remove " + juce::String(type), [type](ClipCell, const ClipRef& clip) {
                std::vector<Edit> edits;
                for (const Effect& e : clip.effects)
                    if (e.type == type)
                        edits.push_back(RemoveEffect{e.id});
                return edits;
            }};
}

void Session::repeatLastEdit()
{
    if (!lastClipEdit_ || !selectedClip_)
        return showStatus("Select clips after a clip edit to repeat it on them", true);
    std::vector<Edit> edits;
    int done = 0, skipped = 0;
    for (const ClipCell& cell : clipTargets(*selectedClip_))
        if (const ClipRef* c = clip(cell)) {
            std::vector<Edit> more = lastClipEdit_->edits(cell, *c);
            if (more.empty())
                ++skipped;
            else
                ++done;
            edits.insert(edits.end(), more.begin(), more.end());
        }
    const juce::String name = lastClipEdit_->name;
    edit(std::move(edits));
    showStatus("Repeated " + name + " on " + juce::String(done) + (done == 1 ? " clip" : " clips") +
                   (skipped ? ", skipped " + juce::String(skipped) + " without it" : juce::String()),
               false);
}

namespace {

// The clip's effect `id`, or when it does not hold it the first effect of `type` with `param`.
const Effect* matching(const ClipRef& clip, uint64_t id, const std::string& type, const std::string& param)
{
    for (const Effect& effect : clip.effects)
        if (effect.id == id)
            return &effect;
    for (const Effect& effect : clip.effects)
        if (effect.type == type && effect.params.contains(param))
            return &effect;
    return nullptr;
}

} // namespace

void Session::setParam(const ParamRef& ref, double value)
{
    const auto owner = ref.effect ? ownerOf(*composition_, ref) : std::nullopt;
    const Effect* effect = ref.effect ? effectById(*composition_, ref.effect) : nullptr;
    if (!owner || !owner->column || !effect)
        return client_.edit(azl::ui::setParam(ref, value));
    const std::string type = effect->type;
    const juce::String name = juce::String::fromUTF8(("set " + type + " " + ref.param).c_str());
    editClips({owner->layer, *owner->column}, {name, [ref, type, value](ClipCell, const ClipRef& clip) {
                   const Effect* e = matching(clip, ref.effect, type, ref.param);
                   return e ? std::vector<Edit>{SetEffectParam{e->id, ref.param, value}} : std::vector<Edit>{};
               }});
}

bool Session::mixedParam(const ParamRef& ref) const
{
    const auto owner = ref.effect ? ownerOf(*composition_, ref) : std::nullopt;
    const Effect* effect = ref.effect ? effectById(*composition_, ref.effect) : nullptr;
    if (!owner || !owner->column || !effect)
        return false;
    std::optional<double> seen;
    for (const ClipCell& target : clipTargets({owner->layer, *owner->column}))
        if (const ClipRef* c = clip(target))
            if (const Effect* e = matching(*c, ref.effect, effect->type, ref.param)) {
                const double v = e->params.at(ref.param);
                if (seen && *seen != v)
                    return true;
                seen = v;
            }
    return false;
}

namespace {

// The clip's edit for the colour `ref` names on another clip: the same colour group of its matching effect, or its
// text's colour; none when it has neither.
std::vector<Edit> clipColourEdit(const ClipRef& clip, Session::ClipCell cell, const ColourRef& ref,
                                 const std::string& type, const Hsva& colour)
{
    if (type.empty()) {
        if (!clip.text)
            return {};
        SetClipText edit{cell.layer, cell.column};
        const Rgba c = toRgba(colour);
        (ref.group == kTextColour ? edit.color : edit.outlineColor) = std::array<double, 4>{c.r, c.g, c.b, c.a};
        return {edit};
    }
    const Effect* e = matching(clip, ref.effect, type, ref.group);
    const EffectDef* def = findEffect(type);
    auto group = def ? colourGroup(*def, ref.group) : std::nullopt;
    if (!e || !group)
        return {};
    return {SetEffectParams{e->id, groupParams(*group, colour)}};
}

} // namespace

void Session::setColour(const ColourRef& ref, const Hsva& colour)
{
    const auto owner = ref.effect ? ownerOf(*composition_, {0, ref.effect, ref.group}) : std::nullopt;
    const bool onClip = ref.effect ? owner && owner->column : clip({ref.layer, ref.column}) != nullptr;
    if (!onClip) {
        if (auto edit = colourEdit(*composition_, ref, colour))
            client_.edit(*edit);
        return;
    }
    const Effect* effect = ref.effect ? effectById(*composition_, ref.effect) : nullptr;
    const std::string type = effect ? effect->type : std::string();
    const ClipCell cell = owner ? ClipCell{owner->layer, *owner->column} : ClipCell{ref.layer, ref.column};
    const std::string name = "set " + (type.empty() ? std::string("text") : type) + " " + ref.group;
    editClips(cell, {juce::String::fromUTF8(name.c_str()),
                     [ref, type, colour](ClipCell c, const ClipRef& clip) {
                         return clipColourEdit(clip, c, ref, type, colour);
                     }});
}

void Session::addRecentColour(const Rgba& colour)
{
    const std::string hex = toHex(colour, true);
    std::erase_if(recent_, [&](const Rgba& c) { return toHex(c, true) == hex; });
    recent_.insert(recent_.begin(), colour);
    if (recent_.size() > kRecentColours)
        recent_.resize(kRecentColours);
    notify(Panels);
}

void Session::toggleClip(ClipCell cell)
{
    if (!clip(cell))
        return;
    if (!selection_.contains(cell))
        selection_.insert(cell);
    else if (selection_.size() > 1)
        selection_.erase(cell);
    if (!selection_.contains(cell)) {
        selectedClip_ = *selection_.begin();
        selectedLayer_ = selectedClip_->layer;
    } else {
        selectedClip_ = cell;
        selectedLayer_ = cell.layer;
    }
    anchor_ = selectedClip_;
    chainTab_ = 2;
    updateWatch();
    notify(Selection);
}

std::vector<uint64_t> Session::gridLayers() const
{
    std::vector<uint64_t> ids;
    for (auto it = composition_->layers.rbegin(); it != composition_->layers.rend(); ++it) {
        if (!it->group)
            ids.push_back(it->id);
        for (auto inner = it->layers.rbegin(); inner != it->layers.rend(); ++inner)
            ids.push_back(inner->id);
    }
    return ids;
}

std::vector<Session::ClipCell> Session::deckClips(std::optional<uint64_t> onlyLayer, std::optional<uint64_t> onlyColumn) const
{
    std::vector<ClipCell> cells;
    const Deck* deck = shownDeck();
    for (uint64_t id : gridLayers())
        for (size_t i = 0; deck && i < deck->columns.size(); ++i) {
            const ClipCell cell{id, deck->columns[i].id};
            if ((!onlyLayer || id == *onlyLayer) && (!onlyColumn || cell.column == *onlyColumn) && clip(cell))
                cells.push_back(cell);
        }
    return cells;
}

std::optional<std::pair<int, int>> Session::gridPosition(ClipCell cell) const
{
    const Deck* deck = shownDeck();
    const std::vector<uint64_t> rows = gridLayers();
    const auto row = std::find(rows.begin(), rows.end(), cell.layer);
    if (!deck || row == rows.end())
        return std::nullopt;
    for (size_t i = 0; i < deck->columns.size(); ++i)
        if (deck->columns[i].id == cell.column)
            return std::make_pair(int(row - rows.begin()), int(i));
    return std::nullopt;
}

std::optional<Session::ClipCell> Session::offsetCell(ClipCell cell, int down, int right) const
{
    const auto at = gridPosition(cell);
    const std::vector<uint64_t> rows = gridLayers();
    if (!at)
        return std::nullopt;
    const int row = at->first + down, column = at->second + right;
    if (row < 0 || row >= int(rows.size()) || column < 0 || column >= int(shownDeck()->columns.size()))
        return std::nullopt;
    return ClipCell{rows[size_t(row)], shownDeck()->columns[size_t(column)].id};
}

void Session::selectRange(ClipCell cell)
{
    const auto from = anchor_ ? gridPosition(*anchor_) : std::nullopt;
    const auto to = gridPosition(cell);
    if (!from || !to)
        return selectClip(cell);
    const auto [top, bottom] = std::minmax(from->first, to->first);
    const auto [left, right] = std::minmax(from->second, to->second);
    selection_.clear();
    for (int row = top; row <= bottom; ++row)
        for (int column = left; column <= right; ++column)
            if (const auto c = offsetCell(*anchor_, row - from->first, column - from->second); c && clip(*c))
                selection_.insert(*c);
    if (selection_.empty())
        return selectClip(cell);
    selectedClip_ = selection_.contains(cell) ? cell : *selection_.begin();
    selectedLayer_ = selectedClip_->layer;
    chainTab_ = 2;
    updateWatch();
    notify(Selection);
}

void Session::selectClips(const std::vector<ClipCell>& cells)
{
    if (cells.empty())
        return;
    selectClip(cells.front());
    selection_.insert(cells.begin(), cells.end());
    notify(Selection);
}

void Session::clearClipSelection()
{
    if (!selectedClip_ && selection_.empty())
        return;
    selectedClip_.reset();
    selection_.clear();
    updateWatch();
    notify(Selection);
}

void Session::setMonitorSource(const std::string& source)
{
    monitorSource_ = source;
    updateWatch();
    notify(Selection);
}

void Session::setMapScreen(uint64_t screen)
{
    mapScreen_ = screen;
    updateWatch();
    notify(Selection);
}

void Session::setMapPreviews(std::set<std::string> sources)
{
    if (sources == mapPreviews_)
        return;
    mapPreviews_ = std::move(sources);
    updateWatch();
}

std::string Session::previewSource() const
{
    if (!selectedLayer_)
        return {};
    return "layer:" + std::to_string(*selectedLayer_);
}

void Session::placeClips(uint64_t layerId, int slot, const std::vector<ClipRef>& clips)
{
    const Layer* target = layer(layerId);
    const Deck* deck = shownDeck();
    if (!target || target->group || !deck || clips.empty())
        return;
    client_.beginGesture();
    size_t at = size_t(std::max(0, slot));
    for (const ClipRef& clip : clips) {
        while (slot < 0 && at < deck->columns.size() && target->clips.contains(deck->columns[at].id))
            ++at;
        if (at < deck->columns.size())
            client_.edit(SetSlot{layerId, deck->columns[at].id, clip});
        else
            client_.edit(AddColumn{deck->id, std::nullopt, Column{}, {Cell{layerId, 0, clip}}});
        ++at;
    }
    client_.endGesture();
}

void Session::clickClip(ClipCell cell)
{
    if (client_.queued(cell.layer, cell.column)) {
        client_.cancelTrigger(cell.layer);
        return;
    }
    client_.triggerClip(cell.layer, cell.column);
}

void Session::cancelQueued()
{
    bool any = false;
    forEachLayer(composition_->layers, [&](const Layer& layer) {
        any = any || (layer.playing && client_.queued(layer.id, layer.playing->column));
    });
    if (any)
        client_.cancelTrigger();
}

void Session::setChainTab(int tab)
{
    chainTab_ = tab;
    notify(Panels);
}

ChainOwner Session::chainOwner() const
{
    if (chainTab_ == 2) {
        const auto cell = clipTabCell();
        return cell ? ChainOwner{cell->layer, cell->column} : ChainOwner{0, 0};
    }
    if (chainTab_ == 1)
        return {selectedLayer_.value_or(kNoLayer)};
    return {};
}

std::optional<Session::ClipCell> Session::clipTabCell() const
{
    if (selectedClip_)
        return selectedClip_;
    const Layer* selected = selectedLayer_ ? layer(*selectedLayer_) : nullptr;
    if (!selected || !playingClip(*selected))
        return std::nullopt;
    return ClipCell{selected->id, selected->playing->column};
}

std::optional<double> Session::drivenValue(const ParamRef& ref) const
{
    auto it = modulation_.params.find(keyOf(ref));
    if (it == modulation_.params.end())
        return std::nullopt;
    return it->second;
}

void Session::selectMacro(int slot)
{
    selectedMacro_ = std::clamp(slot, 0, kMacroSlots - 1);
    notify(Panels);
}

void Session::setLearning(bool on)
{
    learning_ = on;
    notify(Panels);
}

void Session::assignToMacro(int slot, const ParamRef& ref)
{
    const auto info = paramInfo(*composition_, ref);
    if (!info)
        return;
    const Macro* macro = macroSlots(*composition_)[size_t(slot)];
    if (macro && std::any_of(macro->targets.begin(), macro->targets.end(),
                             [&](const MacroTarget& t) { return t.param == ref; }))
        return;
    auto [edits, id] = ensureMacro(*composition_, slot);
    client_.beginGesture();
    for (const Edit& edit : edits)
        client_.edit(edit);
    client_.edit(AddMacroTarget{id, std::nullopt, newTarget(*info)});
    client_.endGesture();
}

void Session::removeFromMacro(uint64_t macro, const ParamRef& ref)
{
    for (const Macro& m : composition_->macros)
        for (size_t i = 0; m.id == macro && i < m.targets.size(); ++i)
            if (m.targets[i].param == ref)
                return client_.edit(RemoveMacroTarget{macro, i});
}

void Session::animate(const ParamRef& ref, const std::string& source)
{
    const auto info = paramInfo(*composition_, ref);
    if (!info)
        return;
    if (const Modulator* existing = animationOf(*composition_, ref))
        client_.edit(SetModulator{withSource(*existing, *info, source)});
    else
        client_.edit(AddModulator{std::nullopt, newAnimation(*composition_, *info, source)});
    setAnimationOpen(ref, true);
}

void Session::stopAnimating(const ParamRef& ref)
{
    if (const Modulator* existing = animationOf(*composition_, ref))
        client_.edit(RemoveModulator{existing->id});
}

bool Session::animationOpen(const ParamRef& ref) const
{
    return animOpen_.contains(keyOf(ref));
}

void Session::setAnimationOpen(const ParamRef& ref, bool open)
{
    if (open)
        animOpen_.insert(keyOf(ref));
    else
        animOpen_.erase(keyOf(ref));
    notify(Panels);
}

bool Session::groupOpen(const std::string& key, bool byDefault) const
{
    const auto it = groupsOpen_.find(key);
    return it == groupsOpen_.end() ? byDefault : it->second;
}

void Session::setGroupOpen(const std::string& key, bool open)
{
    groupsOpen_[key] = open;
    if (settings_)
        settings_->setValue("presetGroups", juce::String(Message(groupsOpen_).dump()));
    notify(Panels);
}

void Session::useSettings(juce::PropertiesFile* settings)
{
    {
        std::lock_guard lock(tokensMutex_);
        settings_ = settings;
    }
    midi_->useSettings(settings);
    if (!settings_)
        return;
    if (tokenFor(host_) != token_)
        reconnect();
    const Message saved = Message::parse(settings_->getValue("presetGroups", "{}").toStdString(), nullptr, false);
    if (saved.is_object())
        for (const auto& [key, open] : saved.items())
            if (open.is_boolean())
                groupsOpen_[key] = open.get<bool>();
}

void Session::setStepsOpen(uint64_t effect, bool open)
{
    if (open)
        stepsOpen_.insert(effect);
    else
        stepsOpen_.erase(effect);
    notify(Panels);
}

void Session::setCollapsed(uint64_t effect, bool collapsed)
{
    if (collapsed)
        collapsed_.insert(effect);
    else
        collapsed_.erase(effect);
    notify(Panels);
}

void Session::goToParam(const ParamRef& ref)
{
    const auto owner = ownerOf(*composition_, ref);
    if (!owner)
        return;
    if (owner->column) {
        const Deck* shown = shownDeck();
        const auto inDeck = [&](const Deck& deck) {
            return std::any_of(deck.columns.begin(), deck.columns.end(), [&](const Column& c) { return c.id == *owner->column; });
        };
        if (!shown || !inDeck(*shown))
            for (const Deck& deck : composition_->decks)
                if (inDeck(deck))
                    client_.selectDeck(deck.id);
        selectClip({owner->layer, *owner->column});
    }
    else if (owner->layer)
        selectLayer(owner->layer);
    else
        selectComposition();
    if (ref.effect)
        collapsed_.erase(ref.effect);
    if (animationOf(*composition_, ref))
        animOpen_.insert(keyOf(ref));
    focused_ = ref;
    focusedUntil_ = now() + kFocusSeconds;
    notify(Panels);
}

// "setLayerOpacity" reads "Set layer opacity".
static std::string opLabel(const std::string& op)
{
    std::string label;
    for (char c : op) {
        if (std::isupper(static_cast<unsigned char>(c)) && !label.empty())
            label += ' ', label += char(std::tolower(static_cast<unsigned char>(c)));
        else
            label += label.empty() ? char(std::toupper(static_cast<unsigned char>(c))) : c;
    }
    return label;
}

// The client follows every step from the snapshot's full list on; an op it never learnt reads "Edit".
void Session::trackHistory(const History& h)
{
    auto labels = [](const std::vector<std::string>& ops) {
        std::vector<std::string> out;
        for (const std::string& op : ops)
            out.push_back(op.empty() ? "Edit" : opLabel(op));
        return out;
    };
    past_ = labels(h.past);
    future_ = labels(h.future);
}

void Session::showStatus(const juce::String& text, bool error)
{
    status_ = text;
    statusError_ = error;
    statusUntil_ = now() + kStatusSeconds;
    notify(Status);
}

void Session::engineError(const std::string& message)
{
    {
        std::lock_guard lock(errorMutex_);
        pendingError_ = message;
    }
    post(Status);
}

void Session::post(int changes)
{
    pending_ |= changes;
    triggerAsyncUpdate();
}

void Session::handleAsyncUpdate()
{
    int changes = pending_.exchange(0);
    if (changes & Document) {
        composition_ = client_.composition();
        if (local_)
            local_->show(client_.preparing() ? composition_ : nullptr);
        trackHistory(client_.history());
        if (selectedLayer_ && !layer(*selectedLayer_))
            selectComposition();
        std::erase_if(selection_, [this](const ClipCell& cell) { return !clip(cell) && cell != selectedClip_; });
    }
    if (changes & Media) {
        std::set<std::string> hashes;
        {
            std::lock_guard lock(mediaMutex_);
            hashes.swap(mediaArrived_);
        }
        for (const std::string& hash : hashes) {
            stills_.erase(hash);
            if (local_ && media_->has(hash))
                local_->mediaAdded(hash);
        }
    }
    if (changes & Document)
        reconcileMedia();
    if (changes & (Document | Clock | Media))
        updateWatch();
    if (changes & Connection) {
        previews_.clear();
        if (!client_.connected())
            stills_.clear();
    }
    if (changes & Previews) {
        if (local_)
            local_->setClipPreviews(client_.clipPreviews());
        refreshPreviews();
    }
    if (changes & Modulation)
        modulation_ = client_.modulation();
    if (changes & Status) {
        juce::String error;
        {
            std::lock_guard lock(errorMutex_);
            error.swapWith(pendingError_);
        }
        if (error.isNotEmpty())
            showStatus(error, true);
    }
    notify(changes);
}

void Session::timerCallback()
{
    client_.requestModulation();
    if (++ticks_ % 3 == 0)
        client_.requestClock();
    if (ticks_ % 30 == 0 && media_->reload()) {
        reconcileMedia();
        notify(Media);
    }
    if (ticks_ % 30 == 15 && media_->reloadUploads())
        notify(Media);
    if (focused_ && now() > focusedUntil_) {
        focused_.reset();
        notify(Panels);
    }
    if (status_.isNotEmpty() && now() > statusUntil_) {
        status_.clear();
        notify(Status);
    }
    std::string error = connection_->lastError();
    if (!error.empty() && now() < connectGraceUntil_)
        error.clear(); // A local engine is still coming up; stay on "connecting…" rather than show its errno.
    if (error != connectionError_) {
        connectionError_ = std::move(error);
        notify(Connection);
    }
}

void Session::notify(int changes)
{
    for (View* view : std::vector<View*>(views_))
        view->sessionChanged(changes);
}

void Session::setPreviewsOn(bool on)
{
    if (on == previewsOn_)
        return;
    previewsOn_ = on;
    updateWatch();
    notify(Previews);
}

std::set<std::string> Session::sourcesToWatch() const
{
    if (!previewsOn_)
        return {};
    std::set<std::string> sources{monitorSource_};
    if (const std::string layerSource = previewSource(); !layerSource.empty())
        sources.insert(layerSource);
    if (mapScreen_)
        sources.insert("screen:" + std::to_string(mapScreen_));
    sources.insert(mapPreviews_.begin(), mapPreviews_.end());
    return sources;
}

void Session::connect()
{
    token_ = tokenFor(host_);
    if (localRender_ == LocalRender::On || (localRender_ == LocalRender::Auto && !isLocalHost(host_)))
        startLocalPreviews(localOptions_);
    updateWatch();
    connectGraceUntil_ = isLocalHost(host_) ? now() + kConnectGraceSeconds : 0.0;
    connection_ = std::make_unique<azl::ui::Connection>(client_, host_, port_, surfaces_, token_);
}

void Session::connectTo(const std::string& host, uint16_t port)
{
    if (host == host_ && port == port_)
        return;
    connection_.reset();
    local_.reset();
    client_.forget();
    returnAudio_.stop();
    host_ = host;
    port_ = port;
    composition_ = client_.composition();
    stills_.clear();
    previews_.clear();
    selectedLayer_.reset();
    selectedClip_.reset();
    selection_.clear();
    anchor_.reset();
    lastClipEdit_.reset();
    monitorSource_ = "composition";
    mapScreen_ = 0;
    mapPreviews_.clear();
    modulation_ = {};
    past_.clear();
    future_.clear();
    connectionError_.clear();
    chainTab_ = 0;
    connect();
    if (settings_) {
        settings_->setValue("host", juce::String(host_));
        settings_->saveIfNeeded();
    }
    notify(Venues * 2 - 1);
}

void Session::startVenues(VenueSources sources)
{
    venues_ = std::make_unique<VenueMonitor>(std::move(sources), [this] { post(Venues); });
    if (!settings_)
        return;
    for (const juce::String& typed : juce::StringArray::fromLines(settings_->getValue("typedVenues"))) {
        if (typed.isEmpty())
            continue;
        const auto [host, port] = hostAndPort(typed.toStdString());
        venues_->addManual(host, port);
    }
}

void Session::connectToTyped(const std::string& typed)
{
    const auto [host, port] = hostAndPort(typed);
    if (venues_)
        venues_->addManual(host, port);
    if (settings_) {
        juce::StringArray hosts = juce::StringArray::fromLines(settings_->getValue("typedVenues"));
        hosts.removeEmptyStrings();
        hosts.addIfNotAlreadyThere(juce::String(typed));
        settings_->setValue("typedVenues", hosts.joinIntoString("\n"));
    }
    connectTo(host, port);
}

namespace {

std::string tokenKey(const std::string& host)
{
    return "token." + juce::String(host).toLowerCase().toStdString();
}

} // namespace

std::string Session::tokenFor(const std::string& host) const
{
    std::lock_guard lock(tokensMutex_);
    if (auto it = tokens_.find(tokenKey(host)); it != tokens_.end())
        return it->second;
    return settings_ ? settings_->getValue(juce::String(tokenKey(host))).toStdString() : std::string();
}

void Session::rememberToken(const std::string& host, const std::string& token)
{
    {
        std::lock_guard lock(tokensMutex_);
        tokens_[tokenKey(host)] = token;
    }
    if (settings_) {
        settings_->setValue(juce::String(tokenKey(host)), juce::String(token));
        settings_->saveIfNeeded();
    }
    if (juce::String(host).equalsIgnoreCase(juce::String(host_)) && token != token_)
        reconnect();
}

void Session::reconnect()
{
    connection_.reset();
    local_.reset();
    client_.forget();
    connect();
}

std::optional<VenueEntry> Session::currentVenue() const
{
    if (!venues_)
        return std::nullopt;
    for (const VenueEntry& entry : venues_->entries())
        if (juce::String(entry.venue.host).equalsIgnoreCase(juce::String(host_)) && entry.venue.port == port_)
            return entry;
    return std::nullopt;
}

void Session::startLocalPreviews(const LocalPreviews::Options& options)
{
    try {
        LocalPreviews::Options shared = options;
        shared.shared = media_->library();
        shared.token = token_;
        local_ = std::make_unique<LocalPreviews>(host_, port_, [this] { post(Previews); }, shared);
    } catch (const std::exception& e) {
        DBG("local previews: " << e.what() << "; the engine renders them");
    }
}

bool Session::rendersLocally(const std::string& source) const
{
    if (!local_)
        return false;
    if (source.starts_with("clip:"))
        return media_->has(source.substr(5));
    return std::ranges::all_of(shownMedia(source), [&](const std::string& hash) { return media_->has(hash); });
}

// A layer source shows the media of that layer and the layers in it; the others may show any playing clip's.
std::set<std::string> Session::shownMedia(const std::string& source) const
{
    std::set<std::string> media;
    const ClockState clock = client_.clock();
    if (source.starts_with("layer:")) {
        if (const Layer* shown = layer(std::strtoull(source.c_str() + 6, nullptr, 10)))
            addShownMedia(*shown, clock, media);
        return media;
    }
    for (const Layer& top : composition_->layers)
        addShownMedia(top, clock, media);
    return media;
}

EngineClient& Session::clientFor(const std::string& source)
{
    if (local_ && (localSources_.contains(source) || localClips_.contains(source)))
        return local_->client();
    return client_;
}

void Session::updateWatch()
{
    watched_ = sourcesToWatch();
    std::set<std::string> local, remote;
    for (const std::string& source : watched_)
        (rendersLocally(source) ? local : remote).insert(source);
    client_.watchPreviews(remote);
    if (local_) {
        local_->client().watchPreviews(local);
        local_->setRendering(previewsOn_);
    }
    localSources_ = std::move(local);
    dropUnwatched();
    updateClipWatch();
}

// Only the selected clip that is not playing is watched: the Preview Monitor shows it; the grid shows stills.
std::set<std::string> Session::clipToWatch() const
{
    const Layer* shown = selectedClip_ && previewsOn_ ? layer(selectedClip_->layer) : nullptr;
    if (!shown || client_.playing(selectedClip_->layer, selectedClip_->column))
        return {};
    auto clip = shown->clips.find(selectedClip_->column);
    if (clip == shown->clips.end() || clip->second.hash.empty())
        return {};
    return {clip->second.hash};
}

void Session::updateClipWatch()
{
    const std::set<std::string> hashes = clipToWatch();
    std::set<std::string> sources;
    for (const std::string& hash : hashes)
        sources.insert("clip:" + hash);
    std::set<std::string> local, remote;
    for (const std::string& hash : hashes)
        (rendersLocally("clip:" + hash) ? local : remote).insert(hash);
    client_.watchClipPreviews(remote, cellOf(remote, selectedClip_));
    if (local_)
        local_->client().watchClipPreviews(local, cellOf(local, selectedClip_));
    localClips_.clear();
    for (const std::string& hash : local)
        localClips_.insert("clip:" + hash);
    if (sources == watchedClips_)
        return;
    watchedClips_ = std::move(sources);
    dropUnwatched();
}

void Session::dropUnwatched()
{
    std::erase_if(previews_, [&](const auto& preview) {
        return !watched_.contains(preview.first) && !watchedClips_.contains(preview.first);
    });
}

void Session::refreshPreviews()
{
    std::set<std::string> sources = watched_;
    sources.insert(watchedClips_.begin(), watchedClips_.end());
    for (const std::string& source : sources) {
        auto picture = clientFor(source).preview(source);
        if (!picture)
            continue;
        Picture& shown = previews_[source];
        if (shown.frame == picture->frame && shown.image.isValid())
            continue;
        shown.frame = picture->frame;
        shown.image = imageOf(*picture);
    }
}

std::pair<std::string, uint16_t> hostAndPort(const std::string& typed)
{
    const size_t colon = typed.find(':');
    if (colon == std::string::npos || colon == 0 || typed.find(':', colon + 1) != std::string::npos)
        return {typed, kDefaultPort};
    const std::string digits = typed.substr(colon + 1);
    const bool numeric = std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(uint8_t(c)); });
    if (digits.empty() || digits.size() > 5 || !numeric)
        return {typed, kDefaultPort};
    const int port = std::stoi(digits);
    if (port < 1 || port > 65535)
        return {typed, kDefaultPort};
    return {typed.substr(0, colon), uint16_t(port)};
}

std::string rememberedHost(juce::PropertiesFile& settings, const std::string& given)
{
    if (given.empty())
        return settings.getValue("host", "127.0.0.1").toStdString();
    settings.setValue("host", juce::String(given));
    settings.saveIfNeeded();
    return given;
}

} // namespace azl::ui
