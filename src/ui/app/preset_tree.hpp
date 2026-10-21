#pragma once

#include "ui/app/widgets.hpp"

namespace azl::ui {

struct PresetRow {
    enum class Kind { Section, Effect, Group, Preset, Chain };
    Kind kind = Kind::Preset;
    int depth = 0;
    // What a header's open state is kept under: "section/Classic", "effect/trails", "group/trails/mine".
    std::string key;
    std::string effectType;
    std::string group;
    PresetEntry preset;
    std::string chain;
    juce::String label;
    bool open = false;
    // The presets under a header that the search keeps.
    int count = 0;
};

// A preset's groups in the order they list.
inline const char* const kPresetGroups[] = {"builtin", "imported", "mine"};
// "Built-in Classic", "Imported" or "Mine".
juce::String presetGroupLabel(const std::string& group);

// The preset of the type with this name that wins: mine over imported over built-in; null when there is none.
const PresetEntry* findPreset(const std::vector<PresetEntry>& entries, const std::string& type, const std::string& name);
bool isMine(const std::vector<PresetEntry>& entries, const std::string& type, const std::string& name);
// The effect has left the preset it names: a parameter the preset sets differs.
bool presetModified(const Effect& effect, const std::vector<PresetEntry>& entries);
// The effect a preset makes, naming the preset.
Effect presetEffect(const PresetEntry& preset);
// Saves the effect's settings as a preset of your own and names it as the effect's preset.
void savePresetAs(Session& session, uint64_t effect, const std::string& name);
// What a new effect of the type starts from: its default preset when the type has one, else the type's defaults.
Effect newEffect(EngineClient& client, const std::string& type);
// A chain preset's effects, with fresh ids to come.
std::vector<Effect> chainEffects(const Message& chains, const std::string& name);
// A dragged chain preset: "chain:" and its JSON (name, effects).
juce::String chainDrag(const std::string& name, const Message& effects);
// This machine's presets as a Presets.json file, and a file merged into the engine's presets.
bool exportPresets(Session& session, const juce::File& file);
bool importPresets(Session& session, const juce::File& file);

// The rows of `types` and their presets. In the browser a section per category ("Ours" for our own effects), an effect
// row per type, and under an open effect a header per group that holds presets; on a card only the group headers.
// Under an open group, its presets. A query keeps the effects whose name or a preset's name contains it, with only the
// matching presets (all of them when the effect's name matches), and opens everything it keeps.
std::vector<PresetRow> presetRows(const std::vector<std::string>& types, const std::vector<PresetEntry>& entries,
                                  const juce::String& query, bool card,
                                  const std::function<bool(const std::string& key, bool byDefault)>& isOpen);

// The effects and their presets under collapsible headers, open state kept by the session. Effects and presets drag
// onto a clip, a layer, a selection, the Composition header or the chain; a double-click adds one to the chain shown.
// Given an effect, it lists that effect's presets only, and a click applies one to it.
class PresetTree : public juce::Component, public PointerTips, private Session::View {
public:
    explicit PresetTree(Session& session, std::optional<uint64_t> effect = std::nullopt);
    ~PresetTree() override;

    // The search text, read at every layout; call queryChanged when it changes.
    std::function<juce::String()> query;
    void queryChanged();
    // The rows as laid out, scrolled.
    std::vector<std::pair<juce::Rectangle<int>, PresetRow>> rows() const;
    int contentHeight() const;
    // Add or apply, then Rename and Delete, which only a saved preset takes.
    Menu presetMenu(const PresetRow& row);
    // A chain preset's: add it to the chain shown, rename or delete it; then export and import of the presets file.
    Menu chainMenu(const PresetRow& row);
    // A click on a header row: opens or closes it.
    void toggle(const PresetRow& row);
    // After a click applied a preset to the effect.
    std::function<void()> onApplied;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String tipAt(juce::Point<int> point) override;

private:
    void sessionChanged(int changes) override;
    std::optional<std::pair<juce::Rectangle<int>, PresetRow>> rowAt(juce::Point<int> point) const;
    std::string cardType() const;
    juce::String currentQuery() const;
    void add(const Effect& effect);
    void addChain(const std::string& name);
    void apply(const PresetEntry& preset);
    bool onTriangle(const std::pair<juce::Rectangle<int>, PresetRow>& row, juce::Point<int> point) const;

    Session& session_;
    std::optional<uint64_t> effect_;
    int scroll_ = 0;
    std::optional<std::pair<juce::Rectangle<int>, PresetRow>> pressed_;
};

// An effect card's presets: a search field, the tree and Save as (to Mine); a Classic effect also offers the import.
class PresetPanel : public juce::Component {
public:
    PresetPanel(Session& session, uint64_t effect);
    void paint(juce::Graphics&) override;
    void resized() override;
    PresetTree& tree() { return tree_; }
    juce::TextEditor& search() { return search_; }
    juce::TextButton& saveAs() { return saveAs_; }

private:
    juce::TextEditor search_;
    PresetTree tree_;
    juce::TextButton saveAs_{"Save as..."}, import_{"Import"};
};

} // namespace azl::ui
