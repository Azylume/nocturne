#include "ui/app/chain_pane.hpp"

#include "document/modulation_edits.hpp"
#include "effects/effect_registry.hpp"
#include "effects/isf.hpp"
#include "ui/app/look.hpp"

#include <span>

namespace azl::ui {

namespace {

constexpr int kRow = 20;
constexpr int kGap = 4;
const int kTransitionBeats[] = {0, 1, 2, 4, 8, 16};
const int kBars[] = {1, 2, 4, 8, 16, 32, 64};
const int kHoldBars[] = {0, 4, 8, 16, 32, 64};
const int kQuantise[] = {0, 1, 2, 4, 8, 16};
const int kClipBeats[] = {0, 1, 2, 4, 8, 16, 32, 64};

template <typename T, size_t N>
int indexOf(const T (&values)[N], T value)
{
    for (size_t i = 0; i < N; ++i)
        if (values[i] == value)
            return int(i);
    return -1;
}

juce::String beatsName(int beats)
{
    return juce::String(beats) + (beats == 1 ? " beat" : " beats");
}

juce::String barsName(int bars)
{
    return juce::String(bars) + (bars == 1 ? " bar" : " bars");
}

// What every clip the Clip tab edits has; none when they differ.
template <typename T>
std::optional<T> shared(const Session& session, Session::ClipCell cell, const std::function<T(const ClipRef&)>& value)
{
    std::optional<T> seen;
    for (const Session::ClipCell& target : session.clipTargets(cell))
        if (const ClipRef* clip = session.clip(target)) {
            const T v = value(*clip);
            if (seen && *seen != v)
                return std::nullopt;
            seen = v;
        }
    return seen;
}

// What every text clip the Clip tab edits has; none when they differ.
template <typename T>
std::optional<T> sharedText(const Session& session, Session::ClipCell cell, const std::function<T(const TextClip&)>& value)
{
    std::optional<T> seen;
    for (const Session::ClipCell& target : session.clipTargets(cell))
        if (const ClipRef* clip = session.clip(target); clip && clip->text) {
            const T v = value(*clip->text);
            if (seen && *seen != v)
                return std::nullopt;
            seen = v;
        }
    return seen;
}

// One SetClipText per text clip the Clip tab edits, as one undo step.
void editText(Session& session, Session::ClipCell cell, const juce::String& name,
              std::function<void(SetClipText&, const TextClip&)> set)
{
    session.editClips(cell, {name, [set](Session::ClipCell c, const ClipRef& clip) {
                                 if (!clip.text)
                                     return std::vector<Edit>{};
                                 SetClipText edit{c.layer, c.column};
                                 set(edit, *clip.text);
                                 return std::vector<Edit>{edit};
                             }});
}

const ClipRef* clipOf(const Session& session, const ChainOwner& owner)
{
    const Layer* layer = session.layer(owner.layer);
    if (!layer || !owner.column)
        return nullptr;
    auto it = layer->clips.find(*owner.column);
    return it == layer->clips.end() ? nullptr : &it->second;
}

// What each of the owner card's fields does; a clip's Length and Transition are its own, not the layer's.
void setTips(OwnerCard& card, bool clip)
{
    const std::pair<const char*, juce::String> tips[] = {
        {"Quantise", "When clip and column triggers start: at once, or on the next beat, half bar, bar or bars"},
        {"Frame guard", "When frames run late, the engine may draw layers at half resolution to keep up"},
        {"Trigger", "When this clip starts: the composition's quantisation, at once, or on the next beat or bars"},
        {"Sync", "BPM: the clip stretches to its length in beats on the beat clock; Timeline: its own frame rate"},
        {"Play", "Loop, bounce, play once and clear or hold the last frame, loop backward, or jump on the beat grid"},
        {"Length", clip ? "The clip's length in beats when synced to BPM; Auto puts it between 80 and 160 BPM"
                        : "How many beats the transition takes; Cut switches at once"},
        {"Step", "The bars Autopilot plays this clip for; Layer's keeps the layer's Every"},
        {"Transition", clip ? "How many beats this clip takes to come in; Layer's keeps the layer's length"
                            : "How the next clip on this layer comes in: alpha fade, wipe, dissolve, push or an ISF "
                              "transition, whose settings fold under Transition settings"},
        {"Speed", "Playback speed, 0 to 4x: drag sideways or double-click to type"},
        {"In", "Where the clip starts, 0 to 99% of its media: drag sideways or double-click to type"},
        {"Out", "Where the clip ends, 1 to 100% of its media: drag sideways or double-click to type"},
        {"Blend", "How it blends onto the layers below it"},
        {"Resolution", "Render scale: drawn at the machine's render scale; Output size: this layer and those above it "
                       "stay at output size"},
        {"Output", "Composition: the group is part of the composition; Output only: it goes only to its projector"},
        {"Projector", "Send this group to a free output of its own"},
        {"Chain", "Shared: the same effects in every deck; Own: this deck gets its own copy to change"},
        {"Group", "The group this layer is in"},
        {"Autopilot", "Autopilot triggers the next or a random clip of this layer by itself"},
        {"Every", "How often Autopilot moves on, in bars"},
        {"Hold", "After a manual trigger, Autopilot keeps that clip this many bars"},
        {"Size", "The font size, as a share of the composition's height"},
        {"Font", "The font; a missing one falls back to the system font"},
        {"Align", "Which edge of the text sits at X: left, centre or right"},
        {"X", "Where the text sits across the composition, 0 to 100%"},
        {"Y", "Where the text's middle sits down the composition, 0 to 100%"},
        {"Outline", "The outline's width, as a share of the font size; 0 draws none"},
    };
    for (const auto& [label, tip] : tips) {
        if (juce::ComboBox* box = card.choice(label))
            box->setTooltip(tip);
        if (NumberField* field = card.field(label))
            field->setTooltip(tip);
    }
}

} // namespace

EffectCard::EffectCard(Session& session, ChainOwner owner, uint64_t effect)
    : session_(session), owner_(std::move(owner)), effect_(effect), pilot_(session, effect), steps_(session, effect)
{
    presets_.onClick = [this] { showMenu(presets_, presetMenu()); };
    presets_.setTooltip("Save, rename or delete this effect's preset, set it as the default, or browse its presets");
    remove_.setTooltip("Remove the effect");
    remove_.onClick = [this] {
        session_.client().edit(RemoveEffect{effect_});
        if (owner_.column)
            session_.rememberClipEdit(Session::removingEffects(type_));
    };
    bypass_.setTooltip("Bypass: the picture as if the effect were not there; nothing recompiles");
    bypass_.onClick = [this] { session_.client().edit(SetEffectBypass{effect_, !bypassed_}); };
    addAndMakeVisible(bypass_);
    addAndMakeVisible(presets_);
    addAndMakeVisible(remove_);
    addChildComponent(pilot_);
    list_.addAndMakeVisible(steps_);
    body_.setViewedComponent(&list_, false);
    body_.setScrollBarsShown(true, false);
    addAndMakeVisible(body_);
    refresh();
}

int EffectCard::cardWidth() const
{
    return session_.collapsed(effect_) ? kCollapsedWidth : kWidth;
}

juce::Rectangle<int> EffectCard::chevronArea() const
{
    const auto header = getLocalBounds().removeFromTop(kHeader);
    const int size = Look::metrics().chevronBox;
    return session_.collapsed(effect_) ? header.withSizeKeepingCentre(size, size) : chevronBox(header);
}

GroupHeader::GroupHeader(std::string name, std::function<void()> onClick)
    : name_(std::move(name)), onClick_(std::move(onClick))
{
    setTooltip(utf8(name_) + ": click to show or hide its switches");
}

void GroupHeader::show(bool open, int on, int count)
{
    open_ = open;
    on_ = on;
    count_ = count;
    repaint();
}

void GroupHeader::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    auto area = getLocalBounds();
    drawChevron(g, chevronBox(area), open_, p.text);
    area.removeFromLeft(Look::metrics().chevronBox);
    g.setFont(Look::font());
    g.setColour(p.muted);
    drawText(g, juce::String(on_) + " of " + juce::String(count_) + " on", area.removeFromRight(area.getWidth() / 2 - 8),
             juce::Justification::centredRight);
    g.setColour(p.text);
    drawText(g, utf8(name_), area, juce::Justification::centredLeft, true);
}

void GroupHeader::mouseUp(const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu() && getLocalBounds().contains(e.getPosition()))
        onClick_();
}

ParamRow* EffectCard::row(const std::string& param)
{
    for (auto& r : rows_)
        if (r->ref().param == param)
            return r.get();
    return nullptr;
}

GroupHeader* EffectCard::groupHeader(const std::string& group)
{
    for (auto& [header, at] : groups_)
        if (header->name() == group)
            return header.get();
    return nullptr;
}

SwatchRow* EffectCard::swatchRow(const std::string& group)
{
    for (auto& [swatch, at] : swatches_)
        if (swatch->swatch().ref().group == group)
            return swatch.get();
    return nullptr;
}

void EffectCard::refresh()
{
    const auto* chain = chainOf(session_.composition(), owner_);
    const Effect* effect = nullptr;
    for (size_t i = 0; chain && i < chain->size(); ++i)
        if ((*chain)[i].id == effect_)
            effect = &(*chain)[i];
    if (!effect)
        return;
    type_ = effect->type;
    bypassed_ = effect->bypass;
    bypass_.setToggleState(bypassed_, juce::dontSendNotification);
    if (rows_.empty())
        if (const EffectDef* def = findEffect(type_))
            buildRows(*def);
    for (auto& r : rows_)
        r->refresh();
    for (auto& [swatch, at] : swatches_)
        swatch->refresh();
    for (auto& [header, at] : groups_) {
        int on = 0, count = 0;
        for (size_t i = at; i < rows_.size() && rowGroups_[i] == header->name(); ++i, ++count)
            if (const auto info = paramInfo(session_.composition(), rows_[i]->ref()); info && info->def)
                on += optionIndex(*info->def, info->value) == 1;
        header->show(openGroups_.contains(header->name()), on, count);
    }
    const bool collapsed = session_.collapsed(effect_);
    presets_.setVisible(!collapsed);
    remove_.setVisible(!collapsed);
    body_.setVisible(!collapsed);
    pilot_.setVisible(!collapsed && !effect->steps.empty());
    pilot_.repaint();
    steps_.repaint();
    resized();
    repaint();
}

void EffectCard::refreshClock()
{
    steps_.refreshClock();
}

void EffectCard::buildRows(const EffectDef& def)
{
    for (const ParamDef& param : def.params) {
        if (!param.group.empty() && (rowGroups_.empty() || rowGroups_.back() != param.group)) {
            const std::string group = param.group;
            groups_.emplace_back(std::make_unique<GroupHeader>(group, [this, group] {
                                     if (!openGroups_.erase(group))
                                         openGroups_.insert(group);
                                     refresh();
                                 }),
                                 rows_.size());
            list_.addAndMakeVisible(*groups_.back().first);
        }
        rows_.push_back(std::make_unique<ParamRow>(session_, ParamRef{0, effect_, param.name}));
        rowGroups_.push_back(param.group);
        list_.addAndMakeVisible(*rows_.back());
    }
    for (const ColourGroup& group : colourGroups(def)) {
        const auto first = std::find_if(def.params.begin(), def.params.end(),
                                        [&](const ParamDef& p) { return p.name == group.key; });
        swatches_.emplace_back(std::make_unique<SwatchRow>(session_, ColourRef{effect_, group.key}),
                               size_t(first - def.params.begin()));
        list_.addAndMakeVisible(*swatches_.back().first);
    }
}

void EffectCard::resized()
{
    if (session_.collapsed(effect_)) {
        const int size = Look::metrics().chevronBox;
        bypass_.setBounds(chevronArea().translated(0, kHeader).withSizeKeepingCentre(size, size));
        return;
    }
    auto header = getLocalBounds().removeFromTop(kHeader);
    remove_.setBounds(header.removeFromRight(20));
    header.removeFromRight(1);
    bypass_.setBounds(header.removeFromRight(20));
    header.removeFromRight(1);
    presets_.setBounds(header.removeFromRight(52));
    header.removeFromRight(1);
    pilot_.setBounds(header.removeFromRight(PilotBadge::kWidth));
    body_.setBounds(getLocalBounds().withTrimmedTop(kHeader + 3));
    const int width = body_.getMaximumVisibleWidth();
    steps_.setBounds(0, 0, width, steps_.preferredHeight(width));
    int y = steps_.getHeight();
    for (size_t i = 0; i < rows_.size(); ++i) {
        for (auto& [swatch, at] : swatches_)
            if (at == i) {
                swatch->setBounds(0, y, width, SwatchRow::kHeight);
                y += SwatchRow::kHeight;
            }
        for (auto& [header, at] : groups_)
            if (at == i) {
                header->setBounds(0, y, width, GroupHeader::kHeight);
                y += GroupHeader::kHeight;
            }
        const bool shown = rowGroups_[i].empty() || openGroups_.contains(rowGroups_[i]);
        rows_[i]->setVisible(shown);
        if (!shown)
            continue;
        rows_[i]->setBounds(0, y, width, rows_[i]->preferredHeight());
        y += rows_[i]->preferredHeight();
    }
    list_.setSize(width, y);
}

void EffectCard::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setColour(p.cell);
    g.fillRect(getLocalBounds());
    if (session_.collapsed(effect_)) {
        g.setColour(bypassed_ ? p.button : p.liveRow);
        g.fillRect(getLocalBounds());
        drawChevron(g, chevronArea(), false, bypassed_ ? p.muted : p.white);
        juce::Graphics::ScopedSaveState state(g);
        g.addTransform(juce::AffineTransform::rotation(-juce::MathConstants<float>::halfPi, 0.0f, 0.0f)
                           .translated(0.0f, float(getHeight())));
        g.setColour(p.white);
        g.setFont(Look::font(true));
        drawText(g, utf8(type_), juce::Rectangle<int>(8, 0, getHeight() - 16 - 2 * kHeader, getWidth()),
                 juce::Justification::centredLeft);
        return;
    }
    auto header = getLocalBounds().removeFromTop(kHeader);
    g.setColour(bypassed_ ? p.button : p.liveRow);
    g.fillRect(header);
    drawChevron(g, chevronArea(), true, bypassed_ ? p.muted : p.white);
    header.removeFromRight(96 + (pilot_.isVisible() ? PilotBadge::kWidth + 1 : 0));
    header.setLeft(chevronArea().getRight());
    header.removeFromRight(8);
    if (const juce::String preset = presetLabel(); preset.isNotEmpty()) {
        g.setFont(Look::smallFont());
        g.setColour(preset.endsWith("*") ? p.accent : p.text);
        const int width = std::min(juce::GlyphArrangement::getStringWidthInt(Look::smallFont(), preset),
                                   header.getWidth() / 2);
        drawText(g, preset, header.removeFromRight(width), juce::Justification::centredRight, true);
        header.removeFromRight(4);
    }
    g.setFont(Look::font(true));
    g.setColour(bypassed_ ? p.muted : p.white);
    drawText(g, headerText(), header, juce::Justification::centredLeft, true);
}

void EffectCard::mouseDown(const juce::MouseEvent&)
{
    dragging_ = false;
}

void EffectCard::mouseDrag(const juce::MouseEvent& e)
{
    if (dragging_ || e.getMouseDownY() >= kHeader + (session_.collapsed(effect_) ? getHeight() : 0) ||
        e.getDistanceFromDragStart() < 5)
        return;
    dragging_ = true;
    startDrag(*this, "fxcard:" + juce::String(int64_t(effect_)), getLocalBounds(), e);
}

void EffectCard::mouseUp(const juce::MouseEvent& e)
{
    const bool header = session_.collapsed(effect_) || e.getMouseDownY() < kHeader;
    if (!dragging_ && header && e.mods.isPopupMenu())
        showMenu(*this, cardMenu());
    else if (!dragging_ && header)
        session_.setCollapsed(effect_, !session_.collapsed(effect_));
    dragging_ = false;
}

Menu EffectCard::cardMenu()
{
    const Composition& composition = session_.composition();
    const auto* chain = chainOf(composition, owner_);
    const Effect* effect = effectById(composition, effect_);
    if (!chain || !effect)
        return {};
    auto at = std::find_if(chain->begin(), chain->end(), [&](const Effect& e) { return e.id == effect_; });
    const size_t after = size_t(at - chain->begin()) + 1;
    Session& session = session_;
    const ChainOwner owner = owner_;
    const uint64_t id = effect_;
    return {{.label = "Copy effect",
             .run = [&session, copy = *effect] { session.copiedEffects = {copy}; },
             .tip = "Copy this effect with its settings, to paste onto a clip, a layer or a chain"},
            {.label = "Paste effects",
             .run = [&session, owner, after] {
                 std::vector<Edit> edits;
                 for (size_t i = 0; i < session.copiedEffects.size(); ++i) {
                     Effect copy = session.copiedEffects[i];
                     copy.id = 0;
                     edits.push_back(addEffectTo(session.composition(), owner, after + i, copy));
                 }
                 session.edit(std::move(edits));
             },
             .enabled = !session_.copiedEffects.empty(),
             .tip = "Insert the copied effects right after this one"},
            {.label = "Duplicate",
             .run = [&session, owner, id, after] {
                 if (auto edit = moveEffectTo(session.composition(), id, owner, after, true))
                     session.client().edit(*edit);
             },
             .tip = "Insert a copy of this effect right after it"},
            {.label = "Bypass",
             .run = [&session, id, bypass = !effect->bypass] { session.client().edit(SetEffectBypass{id, bypass}); },
             .ticked = effect->bypass,
             .tip = "The picture as if the effect were not there; nothing recompiles"},
            {},
            {.label = "Remove",
             .run = [&session, id] { session.client().edit(RemoveEffect{id}); },
             .tip = "Remove the effect"}};
}

juce::String EffectCard::headerText() const
{
    return utf8(type_);
}

juce::String EffectCard::presetLabel() const
{
    const Effect* effect = effectById(session_.composition(), effect_);
    if (!effect || effect->preset.empty())
        return {};
    return utf8(effect->preset) + (presetModified(*effect, session_.client().presetEntries()) ? "*" : "");
}

void EffectCard::saveAs(const std::string& name)
{
    savePresetAs(session_, effect_, name);
}

void EffectCard::renamePreset(const std::string& name)
{
    const Effect* effect = effectById(session_.composition(), effect_);
    if (!effect || name.empty() || !isMine(session_.client().presetEntries(), effect->type, effect->preset))
        return;
    session_.client().renamePreset(effect->type, effect->preset, name);
    session_.client().edit(SetEffectParams{effect_, {}, name});
}

Menu EffectCard::presetMenu()
{
    const Effect* found = effectById(session_.composition(), effect_);
    if (!found)
        return {};
    const Effect effect = *found;
    const auto entries = session_.client().presetEntries();
    const bool named = !effect.preset.empty() && findPreset(entries, effect.type, effect.preset);
    const bool mine = isMine(entries, effect.type, effect.preset);
    const auto defaults = session_.client().presetDefaults();
    const auto current = defaults.find(effect.type);
    const bool isDefault = current != defaults.end() && current->second == effect.preset;
    const juce::String onlyMine = "Built-in and imported presets are read-only: Save as makes your own";
    Session& session = session_;
    const uint64_t id = effect_;
    auto backToDefault = [&session, id, type = effect.type] {
        const Effect start = newEffect(session.client(), type);
        std::map<std::string, double> params;
        if (const EffectDef* def = findEffect(type))
            for (const ParamDef& p : def->params)
                params[p.name] = start.params.contains(p.name) ? start.params.at(p.name) : p.defaultValue;
        session.client().edit(SetEffectParams{id, params, start.preset});
    };
    return {{.label = "Save",
             .run = [this, name = effect.preset] { saveAs(name); },
             .enabled = mine,
             .tip = mine ? "Overwrite the preset " + utf8(effect.preset) + " with these settings" : onlyMine},
            {.label = "Save as...",
             .run = [this, name = effect.preset] {
                 askName("Save preset", "Save these settings as a preset of your own", utf8(name),
                         [this](const juce::String& n) { saveAs(n.toStdString()); });
             },
             .tip = "Save these settings under a name, to Mine on this machine"},
            {.label = "Rename...",
             .run = [this, name = effect.preset] {
                 askName("Rename preset", "New name of the preset", utf8(name),
                         [this](const juce::String& n) { renamePreset(n.toStdString()); });
             },
             .enabled = mine,
             .tip = mine ? juce::String("Give the preset another name") : onlyMine},
            {.label = "Delete",
             .run = [&session, id, type = effect.type, name = effect.preset] {
                 session.client().deletePreset(type, name);
                 session.client().edit(SetEffectParams{id, {}, std::string()});
             },
             .enabled = mine,
             .tip = mine ? juce::String("Remove the preset from this machine; the settings stay") : onlyMine},
            {},
            {.label = "Back to default",
             .run = backToDefault,
             .tip = "Set every parameter to what a new " + utf8(effect.type) + " starts from"},
            {.label = isDefault ? "Clear default" : "Set as default",
             .run = [&session, type = effect.type, name = isDefault ? std::string() : effect.preset] {
                 session.client().setDefaultPreset(type, name);
             },
             .enabled = named,
             .ticked = isDefault,
             .tip = isDefault ? "New " + utf8(effect.type) + " effects start from the type's defaults again"
                              : "New " + utf8(effect.type) + " effects start from this preset"},
            {},
            {.label = "Browse presets...",
             .run = [this] { showPresets(); },
             .tip = "This effect's presets by group: apply one, rename or delete your own"}};
}

juce::String EffectCard::tipAt(juce::Point<int> point)
{
    if (session_.collapsed(effect_) || point.y < kHeader)
        return "Click to fold or unfold; drag to move it in the chain or onto another (Option: copy); right-click for more";
    return {};
}

EffectCard::~EffectCard()
{
    dropPresets(box_.get());
}

// Modal inside the window, so a click outside dismisses it; owned here rather than by JUCE, whose own launcher also
// closes it whenever the app is in the background.
void EffectCard::showPresets()
{
    dropPresets(box_.get());
    juce::Component* parent = getTopLevelComponent();
    panel_ = std::make_unique<PresetPanel>(session_, effect_);
    panel_->tree().onApplied = [this] { closePresets(); };
    box_ = std::make_unique<juce::CallOutBox>(*panel_, parent->getLocalArea(this, presets_.getBounds()), parent);
    const juce::CallOutBox* box = box_.get();
    box_->enterModalState(true, juce::ModalCallbackFunction::create([safe = SafePointer<EffectCard>(this), box](int) {
        juce::MessageManager::callAsync([safe, box] {
            if (safe)
                safe->dropPresets(box);
        });
    }));
}

void EffectCard::closePresets()
{
    if (box_)
        box_->dismiss();
}

void EffectCard::dropPresets(const juce::CallOutBox* box)
{
    if (!box_ || box_.get() != box)
        return;
    if (box_->isCurrentlyModal())
        box_->exitModalState(0);
    box_.reset();
    panel_.reset();
}

OwnerCard::OwnerCard(Session& session) : session_(session)
{
    list_.painter = [this](juce::Graphics& g) { paintRows(g); };
    list_.setInterceptsMouseClicks(false, true);
    body_.setViewedComponent(&list_, false);
    body_.setScrollBarsShown(true, false);
    addAndMakeVisible(body_);
}

// What the card's controls depend on besides the owner: whether its clip (and of which kind) or layer exists and the
// groups to offer.
std::string OwnerCard::signature() const
{
    const ClipRef* clip = clipOf(session_, owner_);
    std::string s;
    if (clip)
        s = clip->text ? "text" : isGeneratorClip(*clip) ? "generator" + std::to_string(clip->effects.front().id) : "clip";
    if (const Layer* layer = session_.layer(owner_.layer)) {
        s += layer->group ? "group" : "layer";
        const Composition& c = session_.composition();
        s += "|deck" + std::to_string(c.deck) + "/" + std::to_string(c.decks.size());
        if (auto own = layer->decks.find(c.deck); own != layer->decks.end())
            s += nlohmann::json(own->second).dump();
        s += "|" + transitionName(deckTransition(*layer, c.deck).mode);
        const auto transitions = isfTransitions();
        for (const IsfTransition& t : *transitions)
            s += "|" + t.name + std::to_string(t.params.size());
    }
    for (const Layer& g : session_.composition().layers)
        if (g.group)
            s += "|" + std::to_string(g.id) + g.name;
    for (const OutputDisplay& d : session_.client().displays())
        s += "|" + d.id;
    for (const Screen& screen : session_.composition().screens)
        s += "|" + screen.display;
    return s;
}

void OwnerCard::setOwner(const ChainOwner& owner)
{
    if (owner == owner_ && built_ && signature() == signature_)
        return;
    owner_ = owner;
    signature_ = signature();
    rebuild();
}

std::pair<juce::String, juce::String> OwnerCard::title() const
{
    if (const ClipRef* clip = clipOf(session_, owner_)) {
        const size_t count = session_.clipTargets({owner_.layer, *owner_.column}).size();
        return {utf8(clip->name) + (count > 1 ? " +" + juce::String(count - 1) : juce::String()), count > 1 ? "clips" : "clip"};
    }
    if (owner_.column)
        return {"No clip", "clip"};
    if (owner_.layer == 0)
        return {"Composition", {}};
    const Layer* layer = session_.layer(owner_.layer);
    if (!layer)
        return {"No layer", "layer"};
    return {utf8(layer->name), layer->group ? "group" : "layer"};
}

void OwnerCard::addChoice(const juce::String& label, const juce::StringArray& items, std::function<int()> current,
                          std::function<void(int)> pick)
{
    auto box = std::make_unique<juce::ComboBox>();
    box->addItemList(items, 1);
    juce::ComboBox* raw = box.get();
    box->onChange = [raw, pick, current] {
        const int index = raw->getSelectedItemIndex();
        if (index >= 0 && index != current())
            pick(index);
    };
    list_.addAndMakeVisible(*box);
    lines_.push_back({label, {raw}, false, fine_});
    choices_.push_back({label, std::move(box), std::move(current)});
}

void OwnerCard::addDeckChoice(const juce::String& label, juce::StringArray items, std::function<int()> shared,
                              std::function<void(int)> pickShared,
                              std::function<std::optional<int>(const DeckLayer&)> get,
                              std::function<void(DeckLayer&, std::optional<int>)> set)
{
    const Composition& composition = session_.composition();
    const Layer* layer = session_.layer(owner_.layer);
    auto deck = std::find_if(composition.decks.begin(), composition.decks.end(),
                             [&](const Deck& d) { return d.id == composition.deck; });
    if (!layer || deck == composition.decks.end())
        return;
    auto found = layer->decks.find(deck->id);
    const DeckLayer settings = found == layer->decks.end() ? DeckLayer{} : found->second;
    const std::optional<int> own = get(settings);
    const int count = items.size();
    if (composition.decks.size() > 1)
        items.add(own ? juce::String("Back to shared") : "Own in " + utf8(deck->name));
    Session& session = session_;
    const uint64_t layerId = layer->id;
    const uint64_t deckId = deck->id;
    addChoice(label, items, [own, shared] { return own ? *own : shared(); },
              [&session, layerId, deckId, settings, own, count, shared, pickShared, set](int i) {
                  if (i < count && !own)
                      return pickShared(i);
                  DeckLayer changed = settings;
                  set(changed, i < count ? std::optional<int>(i) : own ? std::nullopt : std::optional<int>(shared()));
                  session.client().edit(SetDeckLayer{layerId, deckId, changed});
              });
    choices_.back().own = own.has_value();
}

NumberField& OwnerCard::addField(const juce::String& label, double min, double max, double step,
                                 const juce::String& suffix, std::function<std::optional<double>()> current,
                                 std::function<void(double)> set)
{
    auto field = std::make_unique<NumberField>(session_, min, max, step, suffix);
    field->onChange = std::move(set);
    list_.addAndMakeVisible(*field);
    lines_.push_back({label, {field.get()}, false, fine_});
    fields_.push_back({label, std::move(field), std::move(current)});
    return *fields_.back().field;
}

// A generator clip's Source section: the generator's parameters and colour swatches, as its effect card had them.
void OwnerCard::addGeneratorRows(const Effect& generator)
{
    lines_.push_back({"Source", {}, true});
    const EffectDef* def = findEffect(generator.type);
    if (!def)
        return;
    const std::vector<ColourGroup> groups = colourGroups(*def);
    for (const ParamDef& param : def->params) {
        for (const ColourGroup& group : groups)
            if (group.key == param.name)
                addSwatch(ColourRef{generator.id, group.key});
        sourceRows_.push_back(std::make_unique<ParamRow>(session_, ParamRef{0, generator.id, param.name}));
        list_.addAndMakeVisible(*sourceRows_.back());
        lines_.push_back({.row = sourceRows_.back().get()});
    }
}

ParamRow* OwnerCard::sourceRow(const std::string& param) const
{
    for (const auto& row : sourceRows_)
        if (row->ref().param == param)
            return row.get();
    return nullptr;
}

bool OwnerCard::hasSource() const
{
    return std::any_of(lines_.begin(), lines_.end(), [](const Line& line) { return line.header && line.label == "Source"; });
}

// The layer's ISF transition settings, folded: each parameter as a field, or a choice for an option list.
void OwnerCard::addTransitionRows(const Layer& layer)
{
    const TransitionMode mode = deckTransition(layer, session_.composition().deck).mode;
    const auto transitions = isfTransitions();
    const IsfTransition* transition = findIsfTransition(*transitions, mode);
    if (!transition || transition->params.empty())
        return;
    more_ = std::make_unique<juce::TextButton>();
    more_->setTooltip("The settings of the layer's transition");
    more_->onClick = [this] {
        moreOpen_ = !moreOpen_;
        refresh();
    };
    list_.addAndMakeVisible(*more_);
    lines_.push_back({"Transition settings", {more_.get()}, true});
    Session& session = session_;
    const uint64_t id = layer.id;
    const std::string name = transitionName(mode);
    fine_ = true;
    for (const ParamDef& param : transition->params) {
        auto value = [&session, id, name, param] {
            const Layer* l = session.layer(id);
            if (!l)
                return param.defaultValue;
            auto values = l->transitionParams.find(name);
            if (values == l->transitionParams.end())
                return param.defaultValue;
            auto v = values->second.find(param.name);
            return v == values->second.end() ? param.defaultValue : v->second;
        };
        auto set = [&session, id, mode, param](double v) {
            session.client().edit(SetTransitionParam{id, mode, param.name, v});
        };
        const juce::String label = utf8(param.label.empty() ? param.name : param.label);
        if (param.choice && !param.options.empty()) {
            juce::StringArray options;
            for (const std::string& option : param.options)
                options.add(utf8(option));
            addChoice(label, options, [param, value] { return optionIndex(param, value()); },
                      [param, set](int i) { set(optionValue(param, i)); });
        } else {
            addField(label, param.min, param.max, (param.max - param.min) / 100.0, {},
                     [value] { return std::optional<double>(value()); }, set);
        }
    }
    fine_ = false;
}

// A text clip's Source section: its text, size and colour, with font, alignment, position and outline folded.
void OwnerCard::addTextControls(Session::ClipCell cell)
{
    Session& session = session_;
    auto value = [&session, cell](std::function<double(const TextClip&)> of) {
        return [&session, cell, of] { return sharedText<double>(session, cell, of); };
    };
    more_ = std::make_unique<juce::TextButton>();
    more_->setTooltip("Font, alignment, position and outline");
    more_->onClick = [this] {
        moreOpen_ = !moreOpen_;
        refresh();
    };
    list_.addAndMakeVisible(*more_);
    lines_.push_back({"Source", {more_.get()}, true});

    text_ = std::make_unique<juce::TextEditor>();
    text_->setFont(Look::font());
    text_->setTextToShowWhenEmpty("mixed", Look::palette().muted);
    text_->setTooltip("The text; \\n starts a new line. Enter or leaving the field sets it");
    textCurrent_ = [&session, cell] {
        return sharedText<std::string>(session, cell, [](const TextClip& t) { return t.text; });
    };
    auto commit = [this, &session, cell] {
        const std::string typed = text_->getText().replace("\\n", "\n").toStdString();
        if (typed.empty() || typed == textCurrent_())
            return;
        editText(session, cell, "Text", [typed](SetClipText& e, const TextClip&) { e.text = typed; });
    };
    text_->onReturnKey = commit;
    text_->onFocusLost = commit;
    text_->onEscapeKey = [this] {
        text_->setText(utf8(textCurrent_().value_or("")).replace("\n", "\\n"), false);
        text_->giveAwayKeyboardFocus();
    };
    list_.addAndMakeVisible(*text_);
    lines_.push_back({"Text", {text_.get()}});

    const auto [minSize, maxSize] = textParamRange(TextParam::Size);
    addField("Size", minSize * 100.0, maxSize * 100.0, 0.5, "%", value([](const TextClip& t) { return t.size * 100.0; }),
             [&session, cell](double v) {
                 editText(session, cell, "Size", [v](SetClipText& e, const TextClip&) { e.size = v / 100.0; });
             });
    addSwatch(ColourRef{0, kTextColour, cell.layer, cell.column});

    fine_ = true;
    static const juce::StringArray fonts = juce::Font::findAllTypefaceNames();
    juce::StringArray names = fonts;
    const auto font = sharedText<std::string>(session, cell, [](const TextClip& t) { return t.font; });
    if (font && !names.contains(utf8(*font)))
        names.insert(0, utf8(*font));
    addChoice("Font", names,
              [&session, cell, names] {
                  const auto f = sharedText<std::string>(session, cell, [](const TextClip& t) { return t.font; });
                  return f ? names.indexOf(utf8(*f)) : -1;
              },
              [&session, cell, names](int i) {
                  const std::string chosen = names[i].toStdString();
                  editText(session, cell, "Font", [chosen](SetClipText& e, const TextClip&) { e.font = chosen; });
              });
    addChoice("Align", {"Left", "Centre", "Right"},
              [&session, cell] {
                  return sharedText<int>(session, cell, [](const TextClip& t) { return int(t.align); }).value_or(-1);
              },
              [&session, cell](int i) {
                  editText(session, cell, "Align", [i](SetClipText& e, const TextClip&) { e.align = TextAlign(i); });
              });
    const auto [minPosition, maxPosition] = textParamRange(TextParam::X);
    addField("X", minPosition * 100.0, maxPosition * 100.0, 1.0, "%", value([](const TextClip& t) { return t.x * 100.0; }),
             [&session, cell](double v) {
                 editText(session, cell, "X", [v](SetClipText& e, const TextClip&) { e.x = v / 100.0; });
             });
    addField("Y", minPosition * 100.0, maxPosition * 100.0, 1.0, "%", value([](const TextClip& t) { return t.y * 100.0; }),
             [&session, cell](double v) {
                 editText(session, cell, "Y", [v](SetClipText& e, const TextClip&) { e.y = v / 100.0; });
             });
    addField("Outline", 0.0, 50.0, 1.0, "%", value([](const TextClip& t) { return t.outline * 100.0; }),
             [&session, cell](double v) {
                 editText(session, cell, "Outline", [v](SetClipText& e, const TextClip&) { e.outline = v / 100.0; });
             });
    addSwatch(ColourRef{0, kTextOutline, cell.layer, cell.column});
    for (const juce::String label : {"Font", "Align"})
        choice(label)->setTextWhenNothingSelected("mixed");
    fine_ = false;
}

void OwnerCard::addSwatch(ColourRef ref)
{
    swatches_.push_back(std::make_unique<SwatchRow>(session_, std::move(ref)));
    list_.addAndMakeVisible(*swatches_.back());
    lines_.push_back({.fine = fine_, .swatch = swatches_.back().get()});
}

bool OwnerCard::own(const juce::String& label) const
{
    for (const Choice& c : choices_)
        if (c.label == label)
            return c.own;
    return false;
}

NumberField* OwnerCard::field(const juce::String& label) const
{
    for (const Field& f : fields_)
        if (f.label == label)
            return f.field.get();
    return nullptr;
}

void OwnerCard::saveChain(const std::string& name)
{
    const std::vector<Effect>* chain = chainOf(session_.composition(), owner_);
    if (chain && !name.empty())
        session_.client().saveChainPreset(name, *chain);
}

Menu OwnerCard::chainMenu()
{
    const std::vector<Effect>* chain = chainOf(session_.composition(), owner_);
    Session& session = session_;
    const ChainOwner owner = owner_;
    Menu add;
    const Message chains = session_.client().chainPresets();
    for (const auto& [name, effects] : chains.items())
        add.push_back({.label = utf8(name),
                       .run = [&session, owner, name] {
                           session.edit(addEffectsTo(session.composition(), owner, std::nullopt,
                                                     chainEffects(session.client().chainPresets(), name)));
                       },
                       .tip = "Add its " + juce::String(int(effects.size())) + " effects to the end of this chain"});
    return {{.label = "Save chain as preset...",
             .run = [this] {
                 askName("Save chain preset", "Save this chain's effects under a name", {},
                         [this](const juce::String& name) { saveChain(name.toStdString()); });
             },
             .enabled = chain && !chain->empty(),
             .tip = "Keep these effects with their settings as a chain preset on this machine"},
            {.label = "Add chain preset",
             .enabled = chain && !add.empty(),
             .sub = add,
             .tip = "Add a chain preset's effects to this chain"}};
}

void OwnerCard::mouseUp(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && e.y < EffectCard::kHeader)
        showMenu(*this, chainMenu());
}

SwatchRow* OwnerCard::swatchRow(const std::string& group) const
{
    for (const auto& swatch : swatches_)
        if (swatch->swatch().ref().group == group)
            return swatch.get();
    return nullptr;
}

juce::ComboBox* OwnerCard::choice(const juce::String& label) const
{
    for (const Choice& c : choices_)
        if (c.label == label)
            return c.box.get();
    return nullptr;
}

void OwnerCard::rebuild()
{
    built_ = true;
    lines_.clear();
    choices_.clear();
    fields_.clear();
    swatches_.clear();
    opacity_.reset();
    bpm_.reset();
    text_.reset();
    more_.reset();
    sourceRows_.clear();
    EngineClient& client = session_.client();
    const ChainOwner owner = owner_;
    Session& session = session_;
    auto layer = [&session, owner]() { return session.layer(owner.layer); };

    if (owner.layer == 0 && !owner.column) {
        opacity_ = std::make_unique<ParamRow>(session_, ParamRef{0, 0, "opacity"});
        list_.addAndMakeVisible(*opacity_);
        bpm_ = std::make_unique<NumberField>(session_, kMinBpm, kMaxBpm, 0.1);
        bpm_->onChange = [&client](double bpm) { client.edit(SetTempo{bpm}); };
        bpm_->setTooltip("Tempo, " + juce::String(kMinBpm, 0) + " to " + juce::String(kMaxBpm, 0) +
                         " BPM: drag sideways or double-click to type");
        list_.addAndMakeVisible(*bpm_);
        juce::StringArray quantise{"Off", "Beat", "1/2 Bar", "Bar", "2 Bars", "4 Bars"};
        addChoice("Quantise", quantise, [&session] { return indexOf(kQuantise, session.composition().quantise); },
                  [&client](int i) { client.edit(SetQuantise{kQuantise[i]}); });
        addChoice("Frame guard", {"Off", "On"}, [&session] { return int(session.composition().frameGuard); },
                  [&client](int i) { client.edit(SetFrameGuard{i == 1}); });
    } else if (owner.column) {
        const ClipRef* clip = clipOf(session_, owner);
        if (!clip)
            return resized();
        const Session::ClipCell cell{owner.layer, *owner.column};
        auto choice = [&session, cell](std::function<int(const ClipRef&)> value) {
            return [&session, cell, value] { return shared<int>(session, cell, value).value_or(-1); };
        };
        auto edit = [&session, cell](const juce::String& name,
                                     std::function<Edit(Session::ClipCell, const ClipRef&)> make) {
            session.editClips(cell, {name, [make](Session::ClipCell c, const ClipRef& clip) {
                                         return std::vector<Edit>{make(c, clip)};
                                     }});
        };
        juce::StringArray triggers{"Composition's", "Now", "Beat", "1/2 Bar", "Bar", "2 Bars", "4 Bars"};
        addChoice("Trigger", triggers,
                  choice([](const ClipRef& c) { return c.quantise ? indexOf(kQuantise, *c.quantise) + 1 : 0; }),
                  [edit](int i) {
                      const auto quantise = i == 0 ? std::nullopt : std::optional<int>(kQuantise[i - 1]);
                      edit("Trigger", [quantise](auto c, const ClipRef&) {
                          return SetClipTrigger{c.layer, c.column, quantise};
                      });
                  });
        // Media only: Strobe and Lines keep the beat through their own parameters, not the clip's length.
        const bool media = !clip->hash.empty();
        if (media) {
            addChoice("Sync", {"BPM", "Timeline"},
                      choice([](const ClipRef& c) { return int(c.transport.sync == SyncMode::Timeline); }), [edit](int i) {
                          const SyncMode sync = i == 1 ? SyncMode::Timeline : SyncMode::Bpm;
                          edit("Sync", [sync](auto c, const ClipRef&) {
                              return SetClipTransport{.layer = c.layer, .sync = sync, .column = c.column};
                          });
                      });
            addChoice("Play", {"Loop", "Bounce", "Once, clear", "Once, hold", "Reverse", "Random Jump"},
                      choice([](const ClipRef& c) { return int(c.transport.playMode); }), [edit](int i) {
                          edit("Play", [i](auto c, const ClipRef&) {
                              return SetClipTransport{.layer = c.layer, .playMode = PlayMode(i), .column = c.column};
                          });
                      });
            juce::StringArray beats{"Auto"};
            for (int b : kClipBeats)
                if (b)
                    beats.add(beatsName(b));
            addChoice("Length", beats, choice([](const ClipRef& c) { return indexOf(kClipBeats, c.transport.beats); }),
                      [edit](int i) {
                          edit("Length", [i](auto c, const ClipRef&) {
                              return SetClipTransport{.layer = c.layer, .beats = kClipBeats[i], .column = c.column};
                          });
                      });
        }
        juce::StringArray steps{"Layer's"};
        for (int b : kBars)
            steps.add(barsName(b));
        addChoice("Step", steps, choice([](const ClipRef& c) { return c.step == 0 ? 0 : indexOf(kBars, c.step) + 1; }),
                  [edit](int i) {
                      edit("Step", [i](auto c, const ClipRef&) {
                          return SetClipStep{c.layer, c.column, i == 0 ? 0 : kBars[i - 1]};
                      });
                  });
        juce::StringArray transitions{"Layer's"};
        for (int b : kTransitionBeats)
            transitions.add(b ? beatsName(b) : juce::String("Cut"));
        addChoice("Transition", transitions,
                  choice([](const ClipRef& c) {
                      return c.transition ? indexOf(kTransitionBeats, int(*c.transition)) + 1 : 0;
                  }),
                  [edit](int i) {
                      const auto beats = i == 0 ? std::nullopt : std::optional<double>(kTransitionBeats[i - 1]);
                      edit("Transition", [beats](auto c, const ClipRef&) {
                          return SetClipTransition{c.layer, c.column, beats};
                      });
                  });
        for (Choice& c : choices_)
            c.box->setTextWhenNothingSelected("mixed");
        auto value = [&session, cell](std::function<double(const ClipRef&)> of) {
            return [&session, cell, of] { return shared<double>(session, cell, of); };
        };
        if (media) {
            addField("Speed", 0.0, 4.0, 0.01, "x", value([](const ClipRef& c) { return c.transport.speed; }), [edit](double v) {
                edit("Speed", [v](auto c, const ClipRef&) {
                    return SetClipTransport{.layer = c.layer, .column = c.column, .speed = v};
                });
            });
            // In stays before out by at least a percent.
            addField("In", 0.0, 99.0, 1.0, "%", value([](const ClipRef& c) { return c.transport.in * 100.0; }), [edit](double v) {
                edit("In", [v](auto c, const ClipRef& clip) {
                    return SetClipTransport{.layer = c.layer, .column = c.column,
                                            .in = std::min(v / 100.0, clip.transport.out - 0.01)};
                });
            });
            addField("Out", 1.0, 100.0, 1.0, "%", value([](const ClipRef& c) { return c.transport.out * 100.0; }), [edit](double v) {
                edit("Out", [v](auto c, const ClipRef& clip) {
                    return SetClipTransport{.layer = c.layer, .column = c.column,
                                            .out = std::max(v / 100.0, clip.transport.in + 0.01)};
                });
            });
        }
        if (clip->text)
            addTextControls(cell);
        else if (isGeneratorClip(*clip))
            addGeneratorRows(clip->effects.front());
    } else if (const Layer* l = layer()) {
        opacity_ = std::make_unique<ParamRow>(session_, ParamRef{owner.layer, 0, "opacity"});
        list_.addAndMakeVisible(*opacity_);
        juce::StringArray blends;
        for (int i = 0; i < kBlendModeCount; ++i)
            blends.add(blendModeName(BlendMode(i)));
        if (l->group)
            addChoice("Blend", blends, [layer] { return layer() ? int(layer()->blend) : -1; },
                      [&client, owner](int i) { client.edit(SetLayerBlend{owner.layer, BlendMode(i)}); });
        else
            addDeckChoice(
                "Blend", blends, [layer] { return layer() ? int(layer()->blend) : -1; },
                [&client, owner](int i) { client.edit(SetLayerBlend{owner.layer, BlendMode(i)}); },
                [](const DeckLayer& d) { return d.blend ? std::optional<int>(int(*d.blend)) : std::nullopt; },
                [](DeckLayer& d, std::optional<int> i) {
                    d.blend = i ? std::optional<BlendMode>(BlendMode(*i)) : std::nullopt;
                });
        addChoice("Resolution", {"Render scale", "Output size"},
                  [layer] { return layer() ? int(layer()->fullResolution) : -1; },
                  [&client, owner](int i) { client.edit(SetLayerFullResolution{owner.layer, i == 1}); });
        if (l->group) {
            addChoice("Output", {"Composition", "Output only"}, [layer] { return layer() ? int(layer()->outputOnly) : -1; },
                      [&client, owner](int i) { client.edit(SetGroupOutputOnly{owner.layer, i == 1}); });
            std::vector<std::string> displays;
            juce::StringArray projectors{juce::String::fromUTF8("Assign to a projector\xe2\x80\xa6")};
            const auto& screens = session_.composition().screens;
            for (const OutputDisplay& d : session_.client().displays()) {
                if (std::any_of(screens.begin(), screens.end(), [&](const Screen& s) { return s.display == d.id; }))
                    continue;
                displays.push_back(d.id);
                projectors.add(utf8(d.name.empty() ? d.id : d.name));
            }
            if (!displays.empty())
                addChoice("Projector", projectors, [] { return 0; },
                          [&client, owner, displays](int i) {
                              client.edit(AssignDisplay{owner.layer, displays[size_t(i - 1)]});
                          });
        } else {
            const Composition& composition = session_.composition();
            auto shown = std::find_if(composition.decks.begin(), composition.decks.end(),
                                      [&](const Deck& d) { return d.id == composition.deck; });
            if (composition.decks.size() > 1 && shown != composition.decks.end()) {
                addChoice("Chain", {"Shared", "Own in " + utf8(shown->name)},
                          [layer, &session] {
                              return layer() ? int(&deckEffects(*layer(), session.composition().deck) !=
                                                   &layer()->effects)
                                             : -1;
                          },
                          [&session, owner](int i) {
                              const Layer* l = session.layer(owner.layer);
                              if (!l)
                                  return;
                              const uint64_t deck = session.composition().deck;
                              auto found = l->decks.find(deck);
                              DeckLayer settings = found == l->decks.end() ? DeckLayer{} : found->second;
                              settings.effects.reset();
                              if (i == 1) {
                                  settings.effects = l->effects;
                                  for (Effect& e : *settings.effects)
                                      e.id = 0;
                              }
                              session.client().edit(SetDeckLayer{owner.layer, deck, settings});
                          });
                choices_.back().own = &deckEffects(*l, composition.deck) != &l->effects;
            }
            juce::StringArray transitionNames{"Alpha", "Wipe", "Dissolve", "Push", "Window Slice", "Radial",
                                              "Random Squares"};
            std::vector<TransitionMode> modes;
            for (int i = 0; i < transitionNames.size(); ++i)
                modes.push_back(TransitionMode(i));
            for (const IsfTransition& t : *isfTransitions())
                if (t.user) {
                    modes.push_back(t.mode);
                    transitionNames.add(utf8(t.name) + " (user)");
                }
            auto indexOfMode = [modes](TransitionMode mode) {
                auto it = std::find(modes.begin(), modes.end(), mode);
                return it == modes.end() ? -1 : int(it - modes.begin());
            };
            addDeckChoice(
                "Transition", transitionNames,
                [layer, indexOfMode] { return layer() ? indexOfMode(layer()->transition.mode) : -1; },
                [&client, owner, modes](int i) {
                    client.edit(SetLayerTransition{owner.layer, modes[size_t(i)], std::nullopt});
                },
                [indexOfMode](const DeckLayer& d) {
                    return d.transitionMode ? std::optional<int>(indexOfMode(*d.transitionMode)) : std::nullopt;
                },
                [modes](DeckLayer& d, std::optional<int> i) {
                    d.transitionMode = i && *i >= 0 ? std::optional<TransitionMode>(modes[size_t(*i)]) : std::nullopt;
                });
            juce::StringArray lengths;
            for (int b : kTransitionBeats)
                lengths.add(b ? beatsName(b) : juce::String("Cut"));
            addDeckChoice(
                "Length", lengths,
                [layer] { return layer() ? indexOf(kTransitionBeats, int(layer()->transition.beats)) : -1; },
                [&client, owner](int i) {
                    client.edit(SetLayerTransition{owner.layer, std::nullopt, double(kTransitionBeats[i])});
                },
                [](const DeckLayer& d) {
                    return d.transitionBeats ? std::optional<int>(indexOf(kTransitionBeats, int(*d.transitionBeats)))
                                             : std::nullopt;
                },
                [](DeckLayer& d, std::optional<int> i) {
                    d.transitionBeats = i ? std::optional<double>(kTransitionBeats[*i]) : std::nullopt;
                });
            std::vector<uint64_t> groups{0};
            juce::StringArray groupNames{"None"};
            for (const Layer& g : session_.composition().layers)
                if (g.group) {
                    groups.push_back(g.id);
                    groupNames.add(utf8(g.name));
                }
            if (groups.size() > 1)
                addChoice("Group", groupNames,
                          [&session, owner, groups] {
                              for (const Layer& g : session.composition().layers)
                                  for (const Layer& inner : g.layers)
                                      if (inner.id == owner.layer)
                                          return int(std::find(groups.begin(), groups.end(), g.id) - groups.begin());
                              return 0;
                          },
                          [&session, owner, groups](int i) {
                              const uint64_t parent = groups[size_t(i)];
                              size_t index = session.composition().layers.size();
                              for (const Layer& g : session.composition().layers)
                                  if (g.id == parent)
                                      index = g.layers.size();
                              session.client().edit(MoveLayer{owner.layer, parent, index});
                          });
            addDeckChoice(
                "Autopilot", {"Off", "Next clip", "Random clip"},
                [layer] { return layer() ? int(layer()->autopilot.mode) : -1; },
                [&client, owner](int i) { client.edit(SetLayerAutopilot{owner.layer, AutopilotMode(i), std::nullopt}); },
                [](const DeckLayer& d) {
                    return d.autopilotMode ? std::optional<int>(int(*d.autopilotMode)) : std::nullopt;
                },
                [](DeckLayer& d, std::optional<int> i) {
                    d.autopilotMode = i ? std::optional<AutopilotMode>(AutopilotMode(*i)) : std::nullopt;
                });
            juce::StringArray bars;
            for (int b : kBars)
                bars.add(barsName(b));
            addDeckChoice(
                "Every", bars, [layer] { return layer() ? indexOf(kBars, layer()->autopilot.bars) : -1; },
                [&client, owner](int i) { client.edit(SetLayerAutopilot{owner.layer, std::nullopt, kBars[i]}); },
                [](const DeckLayer& d) {
                    return d.autopilotBars ? std::optional<int>(indexOf(kBars, *d.autopilotBars)) : std::nullopt;
                },
                [](DeckLayer& d, std::optional<int> i) {
                    d.autopilotBars = i ? std::optional<int>(kBars[*i]) : std::nullopt;
                });
            juce::StringArray holds;
            for (int b : kHoldBars)
                holds.add(b ? barsName(b) : juce::String("Off"));
            addChoice("Hold", holds, [layer] { return layer() ? indexOf(kHoldBars, layer()->autopilot.hold) : -1; },
                      [&client, owner](int i) {
                          client.edit(SetLayerAutopilot{owner.layer, std::nullopt, std::nullopt, kHoldBars[i]});
                      });
            addTransitionRows(*l);
        }
    }
    setTips(*this, owner.column.has_value());
    refresh();
    resized();
}

void OwnerCard::refresh()
{
    for (Choice& choice : choices_)
        choice.box->setSelectedItemIndex(choice.current(), juce::dontSendNotification);
    if (opacity_)
        opacity_->refresh();
    if (bpm_)
        bpm_->setValue(session_.composition().bpm);
    for (Field& f : fields_)
        if (!f.field->editing())
            f.field->setValue(f.current());
    for (auto& row : sourceRows_)
        row->refresh();
    for (auto& swatch : swatches_)
        swatch->refresh();
    if (text_ && !text_->hasKeyboardFocus(true))
        text_->setText(utf8(textCurrent_().value_or("")).replace("\n", "\\n"), false);
    if (more_)
        more_->setButtonText(moreOpen_ ? "Less" : "More");
    resized();
    repaint();
    list_.repaint();
}

void OwnerCard::resized()
{
    body_.setBounds(getLocalBounds().withTrimmedTop(EffectCard::kHeader + 3));
    const int width = body_.getMaximumVisibleWidth();
    int y = 0;
    auto take = [&](int height) {
        const juce::Rectangle<int> row(0, y, width, height);
        y += height + 2;
        return row;
    };
    if (opacity_)
        opacity_->setBounds(take(opacity_->preferredHeight()));
    if (bpm_)
        bpm_->setBounds(take(kRow).reduced(8, 1).withTrimmedLeft(76));
    for (Line& line : lines_) {
        const bool shown = !line.fine || moreOpen_;
        for (juce::Component* part : line.parts)
            part->setVisible(shown);
        if (line.swatch)
            line.swatch->setVisible(shown);
        if (!shown)
            continue;
        if (line.row) {
            line.row->setBounds(take(line.row->preferredHeight()));
            continue;
        }
        if (line.swatch) {
            line.swatch->setBounds(take(SwatchRow::kHeight));
            continue;
        }
        line.bounds = take(kRow).reduced(8, 1);
        auto row = line.bounds;
        if (line.header) {
            for (juce::Component* part : line.parts)
                part->setBounds(row.removeFromRight(48));
            continue;
        }
        row.removeFromLeft(76);
        const int count = int(line.parts.size());
        for (int i = 0; i < count; ++i) {
            line.parts[size_t(i)]->setBounds(row.removeFromLeft((row.getWidth() - 2 * (count - 1 - i)) / (count - i)));
            row.removeFromLeft(2);
        }
    }
    list_.setSize(width, y);
}

void OwnerCard::paintRows(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setFont(Look::font());
    g.setColour(p.muted);
    if (bpm_)
        drawText(g, "BPM", bpm_->getBounds().withX(8).withWidth(70), juce::Justification::centredLeft);
    for (const Line& line : lines_) {
        if (line.row || line.swatch || (line.fine && !moreOpen_))
            continue;
        if (line.header) {
            g.setColour(p.muted.withAlpha(0.4f));
            g.fillRect(line.bounds.getX(), line.bounds.getY(), line.bounds.getWidth(), 1);
            g.setFont(Look::font(true));
            g.setColour(p.white);
            drawText(g, line.label, line.bounds, juce::Justification::centredLeft);
            g.setFont(Look::font());
            continue;
        }
        g.setColour(own(line.label) ? p.accent : p.muted);
        drawText(g, line.label, line.bounds.withWidth(70), juce::Justification::centredLeft);
    }
}

void OwnerCard::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setColour(p.cell);
    g.fillRect(getLocalBounds());
    auto header = getLocalBounds().removeFromTop(EffectCard::kHeader);
    g.setColour(juce::Colour(0xff2c2c2c));
    g.fillRect(header);
    const auto [name, kind] = title();
    g.setFont(Look::smallFont());
    g.setColour(p.muted);
    drawText(g, kind, header.reduced(8, 0), juce::Justification::centredRight, true);
    g.setFont(Look::font(true));
    g.setColour(p.white);
    drawText(g, name, header.reduced(8, 0).withTrimmedRight(40), juce::Justification::centredLeft, true);
    g.setFont(Look::font());
    g.setColour(p.muted);
    juce::String hint;
    if (owner_.column && !clipOf(session_, owner_))
        hint = "Select a clip, or a layer that plays one.";
    else if (!owner_.column && owner_.layer != 0 && !session_.layer(owner_.layer))
        hint = "Select a layer.";
    if (hint.isNotEmpty())
        g.drawFittedText(hint, getLocalBounds().withTrimmedTop(EffectCard::kHeader + 6).reduced(8, 0).withHeight(40),
                         juce::Justification::topLeft, 2);
}

ChainContent::ChainContent(Session& session) : session_(session), ownerCard_(session)
{
    addAndMakeVisible(ownerCard_);
}

EffectCard* ChainContent::card(uint64_t effect)
{
    for (auto& c : cards_)
        if (c->effect() == effect)
            return c.get();
    return nullptr;
}

void ChainContent::refreshClock()
{
    for (auto& c : cards_)
        c->refreshClock();
}

void ChainContent::refresh(int height)
{
    const ChainOwner owner = session_.chainOwner();
    if (parked_ && !parked_->dragging())
        parked_.reset();
    if (!(owner == owner_))
        for (auto& c : cards_)
            if (!c->dragging())
                c.reset();
    owner_ = owner;
    ownerCard_.setOwner(owner);
    ownerCard_.refresh();
    std::vector<std::unique_ptr<EffectCard>> next;
    const ClipRef* clip = clipOf(session_, owner);
    first_ = clip && isGeneratorClip(*clip) ? 1 : 0;
    if (const auto* chain = chainOf(session_.composition(), owner))
        for (const Effect& effect : std::span(*chain).subspan(std::min(first_, chain->size()))) {
            auto it = std::find_if(cards_.begin(), cards_.end(), [&](const auto& c) { return c && c->effect() == effect.id; });
            if (it != cards_.end()) {
                next.push_back(std::move(*it));
            } else {
                next.push_back(std::make_unique<EffectCard>(session_, owner, effect.id));
                addAndMakeVisible(*next.back());
            }
            next.back()->refresh();
        }
    for (auto& c : cards_)
        if (c && c->dragging()) {
            c->setVisible(false);
            parked_ = std::move(c);
        }
    cards_ = std::move(next);
    layout(height);
}

void ChainContent::layout(int height)
{
    int x = kGap;
    ownerCard_.setBounds(x, kGap, OwnerCard::kWidth, height - 2 * kGap);
    x += OwnerCard::kWidth + kGap;
    for (auto& c : cards_) {
        c->setBounds(x, kGap, c->cardWidth(), height - 2 * kGap);
        x += c->cardWidth() + kGap;
    }
    setSize(x + 170, height);
    repaint();
}

size_t ChainContent::gapAt(int x) const
{
    for (size_t i = 0; i < cards_.size(); ++i)
        if (x < cards_[i]->getBounds().getCentreX())
            return first_ + i;
    return first_ + cards_.size();
}

void ChainContent::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.panel);
    if (!chainOf(session_.composition(), owner_))
        return;
    const int end = cards_.empty() ? ownerCard_.getRight() : cards_.back()->getRight();
    auto zone = juce::Rectangle<int>(end + kGap, kGap, 160, getHeight() - 2 * kGap);
    g.setColour(p.muted.withAlpha(0.4f));
    g.drawRect(zone, 1);
    g.setColour(p.muted);
    g.setFont(Look::font());
    g.drawFittedText(cards_.empty() ? "No effects yet.\nDrag one here from the Effects tab"
                                    : "Drop an effect here\nor double-click one in Effects",
                     zone.reduced(8), juce::Justification::centred, 3);
    if (dropGap_) {
        const size_t card = *dropGap_ - first_;
        const int x = card < cards_.size() ? cards_[card]->getX() - kGap / 2 - 1 : end + kGap / 2 - 1;
        g.setColour(p.white);
        g.fillRect(x - 1, kGap, 3, getHeight() - 2 * kGap);
    }
}

bool ChainContent::isInterestedInDragSource(const SourceDetails& details)
{
    const juce::String description = details.description.toString();
    return (isEffectDrag(description) || description.startsWith("fxcard:")) &&
           chainOf(session_.composition(), owner_) != nullptr;
}

void ChainContent::itemDragMove(const SourceDetails& details)
{
    const auto gap = gapAt(details.localPosition.x);
    if (gap == dropGap_)
        return;
    dropGap_ = gap;
    repaint();
}

void ChainContent::itemDragExit(const SourceDetails&)
{
    dropGap_.reset();
    repaint();
}

void ChainContent::itemDropped(const SourceDetails& details)
{
    dropGap_.reset();
    repaint();
    drop(details.description.toString(), gapAt(details.localPosition.x));
}

void ChainContent::drop(const juce::String& description, std::optional<size_t> gap)
{
    const juce::String rest = description.fromFirstOccurrenceOf(":", false, false);
    const uint64_t effect = description.startsWith("fxcard:") ? uint64_t(rest.getLargeIntValue()) : 0;
    size_t from = 0;
    while (from < cards_.size() && cards_[from]->effect() != effect)
        ++from;
    const bool within = from < cards_.size();
    from += first_;
    if (owner_.column && !within &&
        dropOnSelection(session_, description, {owner_.layer, *owner_.column}, gap))
        return;
    if (const auto added = draggedEffects(session_, description); !added.empty()) {
        if (chainOf(session_.composition(), owner_))
            session_.edit(addEffectsTo(session_.composition(), owner_, gap, added));
        if (owner_.column && added.size() == 1)
            session_.rememberClipEdit(Session::addingEffect(added.front()));
        return;
    }
    if (!gap || !within || juce::ModifierKeys::currentModifiers.isAltDown()) {
        dropEffectCard(session_, description, owner_, gap);
        return;
    }
    const size_t to = *gap > from ? *gap - 1 : *gap;
    if (to != from)
        session_.client().edit(MoveEffect{effect, to});
}

ChainPane::ChainPane(Session& session)
    : TabbedPane({"Composition", "Layer", "Clip"}), session_(session), content_(session)
{
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(false, true);
    addAndMakeVisible(viewport_);
    session_.addView(this);
    tab_ = session_.chainTab();
}

ChainPane::~ChainPane()
{
    session_.removeView(this);
}

bool ChainPane::isInterestedInDragSource(const SourceDetails& details)
{
    const juce::String description = details.description.toString();
    return isEffectDrag(description) || description.startsWith("fxcard:");
}

void ChainPane::itemDragMove(const SourceDetails& details)
{
    showTabAt(details.localPosition);
}

void ChainPane::itemDropped(const SourceDetails& details)
{
    showTabAt(details.localPosition);
    content_.drop(details.description.toString(), std::nullopt);
}

void ChainPane::resized()
{
    viewport_.setBounds(body());
    refresh();
}

void ChainPane::tabChanged()
{
    session_.setChainTab(tab_);
}

void ChainPane::refresh()
{
    content_.refresh(viewport_.getMaximumVisibleHeight());
    const auto focused = session_.focusedParam();
    if (!focused)
        scrolledTo_.reset();
    if (focused && focused != scrolledTo_) {
        scrolledTo_ = focused;
        if (EffectCard* card = focused->effect ? content_.card(focused->effect) : nullptr)
            viewport_.setViewPosition(std::max(0, card->getX() - 40), 0);
        else
            viewport_.setViewPosition(0, 0);
    }
}

void ChainPane::sessionChanged(int changes)
{
    if (changes & (Session::Selection | Session::Panels))
        tab_ = session_.chainTab();
    if (changes & (Session::Document | Session::Selection | Session::Panels | Session::Connection | Session::Outputs)) {
        refresh();
        repaint();
    } else if (changes & Session::Modulation) {
        refreshDrivenRows(content_);
    }
    if (changes & Session::Clock)
        content_.refreshClock();
}

juce::String ChainPane::tabName(int index) const
{
    if (index == 1)
        if (auto id = session_.selectedLayer())
            if (const Layer* layer = session_.layer(*id))
                return juce::String::fromUTF8("Layer \xc2\xb7 ") + utf8(layer->name);
    if (index == 2)
        if (auto cell = session_.clipTabCell())
            if (const Layer* layer = session_.layer(cell->layer))
                if (auto clip = layer->clips.find(cell->column); clip != layer->clips.end())
                    return juce::String::fromUTF8("Clip \xc2\xb7 ") + utf8(clip->second.name);
    return TabbedPane::tabName(index);
}

} // namespace azl::ui
