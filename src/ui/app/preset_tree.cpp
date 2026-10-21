#include "ui/app/preset_tree.hpp"

#include "effects/effect_registry.hpp"
#include "ui/app/look.hpp"

namespace azl::ui {

namespace {

constexpr int kRow = 22;
constexpr int kIndent = 12;

const char* kindName(StageKind kind)
{
    switch (kind) {
    case StageKind::Pointwise: return "pointwise";
    case StageKind::Remap: return "remap";
    case StageKind::Neighbourhood: return "neighbourhood";
    case StageKind::MultiPass: return "multi-pass";
    }
    return "";
}

PresetRow header(PresetRow::Kind kind, int depth, std::string key, std::string type, juce::String label, bool open)
{
    PresetRow row;
    row.kind = kind;
    row.depth = depth;
    row.key = std::move(key);
    row.effectType = std::move(type);
    row.label = std::move(label);
    row.open = open;
    return row;
}

} // namespace

juce::String presetGroupLabel(const std::string& group)
{
    if (group == "builtin")
        return "Built-in Classic";
    if (group == "imported")
        return "Imported";
    return "Mine";
}

const PresetEntry* findPreset(const std::vector<PresetEntry>& entries, const std::string& type, const std::string& name)
{
    const PresetEntry* found = nullptr;
    for (const char* group : kPresetGroups)
        for (const PresetEntry& entry : entries)
            if (entry.effectType == type && entry.name == name && entry.group == group)
                found = &entry;
    return found;
}

bool isMine(const std::vector<PresetEntry>& entries, const std::string& type, const std::string& name)
{
    const PresetEntry* preset = findPreset(entries, type, name);
    return preset && preset->group == "mine";
}

bool presetModified(const Effect& effect, const std::vector<PresetEntry>& entries)
{
    const PresetEntry* preset = effect.preset.empty() ? nullptr : findPreset(entries, effect.type, effect.preset);
    if (!preset)
        return false;
    for (const auto& [name, value] : preset->params) {
        auto it = effect.params.find(name);
        if (it != effect.params.end() && std::abs(it->second - value) > 1e-6)
            return true;
    }
    return false;
}

Effect presetEffect(const PresetEntry& preset)
{
    return {0, preset.effectType, preset.params, false, preset.name};
}

void savePresetAs(Session& session, uint64_t effect, const std::string& name)
{
    const Effect* current = effectById(session.composition(), effect);
    if (!current || name.empty())
        return;
    session.client().savePreset(effect, name);
    if (current->preset != name)
        session.client().edit(SetEffectParams{effect, {}, name});
}

Effect newEffect(EngineClient& client, const std::string& type)
{
    const auto defaults = client.presetDefaults();
    const auto entries = client.presetEntries();
    auto it = defaults.find(type);
    const PresetEntry* preset = it == defaults.end() ? nullptr : findPreset(entries, type, it->second);
    return preset ? presetEffect(*preset) : Effect{0, type, {}};
}

std::vector<Effect> chainEffects(const Message& chains, const std::string& name)
{
    std::vector<Effect> effects;
    for (const Message& e : chains.value(name, Message::array())) {
        Effect effect = e.get<Effect>();
        effect.id = 0;
        if (findEffect(effect.type))
            effects.push_back(std::move(effect));
    }
    return effects;
}

juce::String chainDrag(const std::string& name, const Message& effects)
{
    return "chain:" + utf8(Message{{"name", name}, {"effects", effects}}.dump());
}

bool exportPresets(Session& session, const juce::File& file)
{
    return file.replaceWithText(utf8(session.client().presetFile().dump(2)));
}

bool importPresets(Session& session, const juce::File& file)
{
    const Message parsed = Message::parse(file.loadFileAsString().toStdString(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("presets")) {
        session.showStatus("Not a presets file: " + file.getFileName(), true);
        return false;
    }
    session.client().mergePresets(parsed);
    return true;
}

std::vector<PresetRow> presetRows(const std::vector<std::string>& types, const std::vector<PresetEntry>& entries,
                                  const juce::String& query, bool card,
                                  const std::function<bool(const std::string& key, bool byDefault)>& isOpen)
{
    const juce::String q = query.trim();
    const bool searching = q.isNotEmpty();
    auto open = [&](const std::string& key, bool byDefault) { return searching || isOpen(key, byDefault); };
    std::vector<PresetRow> rows;
    std::optional<size_t> section;
    for (const std::string& type : types) {
        const bool typeMatches = !searching || utf8(type).containsIgnoreCase(q) || utf8(effectLabel(type)).containsIgnoreCase(q);
        std::vector<const PresetEntry*> kept;
        for (const PresetEntry& entry : entries)
            if (entry.effectType == type && (typeMatches || utf8(entry.name).containsIgnoreCase(q)))
                kept.push_back(&entry);
        if (!typeMatches && kept.empty())
            continue;
        int depth = 0;
        if (!card) {
            const EffectDef* def = findEffect(type);
            const std::string category = def && !def->category.empty() ? def->category : "Ours";
            if (!section || rows[*section].label != utf8(category)) {
                rows.push_back(header(PresetRow::Kind::Section, 0, "section/" + category, {}, utf8(category),
                                      open("section/" + category, true)));
                section = rows.size() - 1;
            }
            ++rows[*section].count;
            if (!rows[*section].open)
                continue;
            PresetRow effect = header(PresetRow::Kind::Effect, 1, "effect/" + type, type, utf8(effectLabel(type)),
                                      !kept.empty() && open("effect/" + type, false));
            effect.count = int(kept.size());
            rows.push_back(effect);
            if (!effect.open)
                continue;
            depth = 2;
        }
        for (const char* group : kPresetGroups) {
            std::vector<const PresetEntry*> inGroup;
            for (const PresetEntry* entry : kept)
                if (entry->group == group)
                    inGroup.push_back(entry);
            if (inGroup.empty())
                continue;
            const std::string key = "group/" + type + "/" + group;
            PresetRow row = header(PresetRow::Kind::Group, depth, key, type, presetGroupLabel(group), open(key, true));
            row.group = group;
            row.count = int(inGroup.size());
            rows.push_back(row);
            if (!row.open)
                continue;
            for (const PresetEntry* entry : inGroup) {
                PresetRow preset = header(PresetRow::Kind::Preset, depth + 1, {}, type, utf8(entry->name), false);
                preset.group = group;
                preset.preset = *entry;
                rows.push_back(preset);
            }
        }
    }
    return rows;
}

namespace {

// The chain presets under a "Chains" section, those whose name holds the query.
std::vector<PresetRow> chainRows(const Message& chains, const juce::String& query,
                                 const std::function<bool(const std::string& key, bool byDefault)>& isOpen)
{
    const juce::String q = query.trim();
    std::vector<PresetRow> rows;
    for (const auto& [name, effects] : chains.items()) {
        if (q.isNotEmpty() && !utf8(name).containsIgnoreCase(q))
            continue;
        if (rows.empty())
            rows.push_back(header(PresetRow::Kind::Section, 0, "section/Chains", {}, "Chains",
                                  q.isNotEmpty() || isOpen("section/Chains", true)));
        ++rows.front().count;
        if (!rows.front().open)
            continue;
        PresetRow row = header(PresetRow::Kind::Chain, 1, {}, {}, utf8(name), false);
        row.chain = name;
        row.count = int(effects.size());
        rows.push_back(row);
    }
    return rows;
}

} // namespace

PresetTree::PresetTree(Session& session, std::optional<uint64_t> effect) : session_(session), effect_(effect)
{
    session_.addView(this);
}

PresetTree::~PresetTree()
{
    session_.removeView(this);
}

void PresetTree::queryChanged()
{
    scroll_ = 0;
    repaint();
}

juce::String PresetTree::currentQuery() const
{
    return query ? query() : juce::String();
}

std::string PresetTree::cardType() const
{
    const Effect* effect = effect_ ? effectById(session_.composition(), *effect_) : nullptr;
    return effect ? effect->type : std::string();
}

std::vector<std::pair<juce::Rectangle<int>, PresetRow>> PresetTree::rows() const
{
    const std::vector<std::string> types = effect_ ? std::vector<std::string>{cardType()} : effectTypes();
    auto isOpen = [this](const std::string& key, bool byDefault) { return session_.groupOpen(key, byDefault); };
    auto all = presetRows(types, session_.client().presetEntries(), currentQuery(), effect_.has_value(), isOpen);
    if (!effect_)
        for (PresetRow& row : chainRows(session_.client().chainPresets(), currentQuery(), isOpen))
            all.push_back(std::move(row));
    std::vector<std::pair<juce::Rectangle<int>, PresetRow>> result;
    int y = -scroll_;
    for (const PresetRow& row : all) {
        result.emplace_back(juce::Rectangle<int>(row.depth * kIndent, y, getWidth() - row.depth * kIndent, kRow), row);
        y += kRow + 1;
    }
    return result;
}

int PresetTree::contentHeight() const
{
    const auto all = rows();
    return all.empty() ? 0 : all.back().first.getBottom() + scroll_;
}

std::optional<std::pair<juce::Rectangle<int>, PresetRow>> PresetTree::rowAt(juce::Point<int> point) const
{
    for (const auto& row : rows())
        if (row.first.contains(point))
            return row;
    return std::nullopt;
}

bool PresetTree::onTriangle(const std::pair<juce::Rectangle<int>, PresetRow>& row, juce::Point<int> point) const
{
    return row.second.kind != PresetRow::Kind::Effect ||
           (row.second.count > 0 && chevronBox(row.first).contains(point));
}

void PresetTree::toggle(const PresetRow& row)
{
    if (row.key.empty() || currentQuery().trim().isNotEmpty())
        return;
    session_.setGroupOpen(row.key, !row.open);
}

void PresetTree::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.setFont(Look::font());
    const auto all = rows();
    if (all.empty()) {
        g.setColour(p.muted);
        drawText(g, effect_ ? "No presets of this effect yet" : "Nothing matches", getLocalBounds().withHeight(kRow),
                 juce::Justification::centred);
        return;
    }
    for (const auto& [area, row] : all) {
        if (!area.intersects(getLocalBounds()))
            continue;
        g.setFont(Look::font());
        switch (row.kind) {
        case PresetRow::Kind::Section:
            drawHeader(g, area, p.header, p.white, row.label, row.open);
            break;
        case PresetRow::Kind::Effect:
            if (row.count > 0)
                drawHeader(g, area, p.cell, p.text, row.label, row.open);
            else
                fillWithText(g, area, p.cell, p.text, row.label, juce::Justification::centredLeft,
                             Look::metrics().chevronBox);
            break;
        case PresetRow::Kind::Group:
            drawHeader(g, area, row.open ? p.button : p.cell, row.open ? p.white : p.text, row.label, row.open);
            break;
        case PresetRow::Kind::Preset:
        case PresetRow::Kind::Chain:
            fillWithText(g, area, p.emptyCell, p.text, row.label, juce::Justification::centredLeft, 8);
            break;
        }
        g.setColour(p.muted);
        g.setFont(Look::smallFont());
        juce::String right;
        if (row.kind == PresetRow::Kind::Effect) {
            const EffectDef* def = findEffect(row.effectType);
            right = juce::String(def ? kindName(def->kind) : "") + (row.count > 0 ? juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(row.count) : "");
        } else if (row.kind != PresetRow::Kind::Preset) {
            right = juce::String(row.count);
        }
        drawText(g, right, area.reduced(6, 0), juce::Justification::centredRight);
    }
}

void PresetTree::mouseDown(const juce::MouseEvent& e)
{
    pressed_ = rowAt(e.getPosition());
    if (!pressed_ || !e.mods.isPopupMenu())
        return;
    const PresetRow row = pressed_->second;
    pressed_.reset();
    if (row.kind == PresetRow::Kind::Preset)
        showMenu(*this, presetMenu(row));
    else if (row.kind == PresetRow::Kind::Chain || row.key == "section/Chains")
        showMenu(*this, chainMenu(row));
    else if (row.key == "section/ISF" || row.key == "section/User")
        showMenu(*this, {{.label = "Rescan user shaders",
                          .run = [this] { session_.client().rescanIsf(); },
                          .tip = "Read the engine machine's isf-user folder again: new, changed and removed shaders "
                                 "take effect"}});
}

void PresetTree::mouseUp(const juce::MouseEvent& e)
{
    const auto pressed = std::move(pressed_);
    pressed_.reset();
    if (!pressed || e.getDistanceFromDragStart() >= 5)
        return;
    if (pressed->second.kind == PresetRow::Kind::Preset) {
        if (effect_)
            apply(pressed->second.preset);
        return;
    }
    if (onTriangle(*pressed, e.getMouseDownPosition()))
        toggle(pressed->second);
}

void PresetTree::mouseDrag(const juce::MouseEvent& e)
{
    if (!pressed_ || e.getDistanceFromDragStart() < 5)
        return;
    const auto& [area, row] = *pressed_;
    if (row.kind == PresetRow::Kind::Preset)
        startDrag(*this, presetDrag(row.preset), area, e);
    else if (row.kind == PresetRow::Kind::Chain)
        startDrag(*this, chainDrag(row.chain, session_.client().chainPresets().value(row.chain, Message::array())),
                  area, e);
    else if (row.kind == PresetRow::Kind::Effect)
        startDrag(*this, "effect:" + utf8(row.effectType), area, e);
    pressed_.reset();
}

void PresetTree::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto row = rowAt(e.getPosition());
    if (effect_ || !row)
        return;
    if (row->second.kind == PresetRow::Kind::Preset)
        add(presetEffect(row->second.preset));
    else if (row->second.kind == PresetRow::Kind::Chain)
        addChain(row->second.chain);
    else if (row->second.kind == PresetRow::Kind::Effect && !onTriangle(*row, e.getPosition()))
        add(newEffect(session_.client(), row->second.effectType));
}

void PresetTree::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    scroll_ = wheelScrolled(scroll_, wheel, contentHeight(), getHeight());
    repaint();
}

void PresetTree::add(const Effect& effect)
{
    const ChainOwner owner = session_.chainOwner();
    if (!chainOf(session_.composition(), owner))
        return;
    session_.client().edit(addEffectTo(session_.composition(), owner, std::nullopt, effect));
    if (owner.column)
        session_.rememberClipEdit(Session::addingEffect(effect));
}

namespace {

// A file picked in the system's dialog: one to save the presets to, or one to read.
void chooseFile(bool save, std::function<void(const juce::File&)> done)
{
    static std::unique_ptr<juce::FileChooser> chooser;
    chooser = std::make_unique<juce::FileChooser>(
        save ? "Export presets" : "Import presets",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Nocturne Presets.json"),
        "*.json");
    const int mode = save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                          : juce::FileBrowserComponent::openMode;
    chooser->launchAsync(mode | juce::FileBrowserComponent::canSelectFiles, [done](const juce::FileChooser& c) {
        if (c.getResult() != juce::File())
            done(c.getResult());
    });
}

} // namespace

void PresetTree::addChain(const std::string& name)
{
    const ChainOwner owner = session_.chainOwner();
    if (chainOf(session_.composition(), owner))
        session_.edit(addEffectsTo(session_.composition(), owner, std::nullopt,
                                   chainEffects(session_.client().chainPresets(), name)));
}

Menu PresetTree::chainMenu(const PresetRow& row)
{
    Session& session = session_;
    const std::string name = row.chain;
    const bool chain = !name.empty();
    return {{.label = "Add to the chain shown",
             .run = [this, name] { addChain(name); },
             .enabled = chain,
             .tip = "Add the chain preset's effects to the end of the chain below"},
            {.label = "Rename...",
             .run = [&session, name] {
                 askName("Rename chain preset", "New name of the chain preset", utf8(name),
                         [&session, name](const juce::String& to) {
                             session.client().renameChainPreset(name, to.toStdString());
                         });
             },
             .enabled = chain,
             .tip = "Give the chain preset another name"},
            {.label = "Delete",
             .run = [&session, name] { session.client().deleteChainPreset(name); },
             .enabled = chain,
             .tip = "Remove the chain preset from this machine"},
            {},
            {.label = "Export presets...",
             .run = [&session] {
                 chooseFile(true, [&session](const juce::File& file) { exportPresets(session, file); });
             },
             .tip = "Save your own presets, defaults and chain presets to a file"},
            {.label = "Import presets...",
             .run = [&session] {
                 chooseFile(false, [&session](const juce::File& file) { importPresets(session, file); });
             },
             .tip = "Add the presets of a file to yours; a preset of the same name is replaced"}};
}

void PresetTree::apply(const PresetEntry& preset)
{
    if (!effect_)
        return;
    session_.client().applyPreset(*effect_, preset.name, preset.group);
    if (onApplied)
        onApplied();
}

Menu PresetTree::presetMenu(const PresetRow& row)
{
    Session& session = session_;
    const PresetEntry preset = row.preset;
    const bool mine = preset.group == "mine";
    Menu menu;
    if (effect_)
        menu.push_back({.label = "Apply", .run = [this, preset] { apply(preset); }, .tip = "Set this effect to the preset"});
    else
        menu.push_back({.label = "Add to the chain shown",
                        .run = [this, preset] { add(presetEffect(preset)); },
                        .tip = "Add the effect with this preset's settings to the chain below"});
    const juce::String onlyMine = "Only your own presets (Mine) can be renamed or deleted";
    menu.push_back({});
    menu.push_back({.label = "Rename...",
                    .run = [&session, preset] {
                        askName("Rename preset", "New name of the " + utf8(preset.effectType) + " preset",
                                utf8(preset.name), [&session, preset](const juce::String& name) {
                                    session.client().renamePreset(preset.effectType, preset.name, name.toStdString());
                                });
                    },
                    .enabled = mine,
                    .tip = mine ? juce::String("Give the preset another name") : onlyMine});
    menu.push_back({.label = "Delete",
                    .run = [&session, preset] { session.client().deletePreset(preset.effectType, preset.name); },
                    .enabled = mine,
                    .tip = mine ? juce::String("Remove the preset from this machine") : onlyMine});
    return menu;
}

juce::String PresetTree::tipAt(juce::Point<int> point)
{
    const auto row = rowAt(point);
    if (!row)
        return effect_ ? "This effect's presets: our built-in looks, imported ones and your own"
                       : "Effects and their presets: drag one onto a clip, a layer or the chain";
    switch (row->second.kind) {
    case PresetRow::Kind::Section: return "Click to fold or unfold these effects";
    case PresetRow::Kind::Group: return "Click to fold or unfold these presets";
    case PresetRow::Kind::Effect:
        return row->second.count > 0 && onTriangle(*row, point)
                   ? "Click to show or hide this effect's presets"
                   : "Drag onto a clip, a layer, the Composition header or the chain; double-click adds it to the "
                     "chain shown";
    case PresetRow::Kind::Chain:
        return "A chain preset: drag onto a clip, a layer or the chain to add its effects; double-click adds them to "
               "the chain shown; right-click for more";
    case PresetRow::Kind::Preset:
        return effect_ ? "Click to apply; drag onto a clip, a layer or the chain; right-click for more"
                       : "Drag onto a clip, a layer or the chain to add the effect with this preset; double-click adds "
                         "it to the chain shown; right-click for more";
    }
    return {};
}

void PresetTree::sessionChanged(int changes)
{
    if (changes & (Session::Presets | Session::Panels | Session::Document | Session::Connection))
        repaint();
}

PresetPanel::PresetPanel(Session& session, uint64_t effect) : tree_(session, effect)
{
    const Effect* found = effectById(session.composition(), effect);
    const std::string type = found ? found->type : std::string();
    search_.setTextToShowWhenEmpty("Search presets", Look::palette().muted);
    search_.setFont(Look::font());
    search_.setTooltip("Show only the presets whose names contain this");
    tree_.query = [this] { return search_.getText(); };
    search_.onTextChange = [this] { tree_.queryChanged(); };
    saveAs_.setTooltip("Save this effect's settings as a preset of your own (Mine)");
    saveAs_.onClick = [&session, effect, type] {
        askName("Save preset", "Name of the " + utf8(type) + " preset", "My " + utf8(type),
                [&session, effect](const juce::String& name) { savePresetAs(session, effect, name.toStdString()); });
    };
    import_.setTooltip("Import presets from installed VJ apps: reads their preset folders; macOS may ask to allow access "
                       "to Documents");
    import_.onClick = [&session] { session.client().importPresets(); };
    const EffectDef* def = findEffect(type);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&search_, &tree_, &saveAs_})
        addAndMakeVisible(c);
    addChildComponent(import_);
    import_.setVisible(def && def->category == "Classic");
    setSize(280, 340);
}

void PresetPanel::paint(juce::Graphics& g)
{
    g.fillAll(Look::palette().panel);
}

void PresetPanel::resized()
{
    auto area = getLocalBounds();
    search_.setBounds(area.removeFromTop(24));
    area.removeFromTop(4);
    auto buttons = area.removeFromBottom(24);
    area.removeFromBottom(4);
    if (import_.isVisible()) {
        import_.setBounds(buttons.removeFromRight(buttons.getWidth() / 2 - 2));
        buttons.removeFromRight(4);
    }
    saveAs_.setBounds(buttons);
    tree_.setBounds(area);
}

} // namespace azl::ui
