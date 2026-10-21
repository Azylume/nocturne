#pragma once

#include "ui/app/widgets.hpp"

#include <optional>
#include <set>
#include <vector>

namespace azl::ui {

// A clip of one of the deck's columns is playing.
bool deckPlaying(const Composition& composition, const Deck& deck);

// The deck tabs: clicking one shows that deck (a live action); a dot marks decks with a clip playing. Dragging one
// reorders the decks. Double-click renames; right-click renames, duplicates or deletes.
class DeckTabs : public juce::Component, public PointerTips, private Session::View {
public:
    explicit DeckTabs(Session& session);
    ~DeckTabs() override;
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;
    Menu deckMenu(uint64_t deck);
    std::vector<std::pair<juce::Rectangle<int>, uint64_t>> tabs() const;
    // What the shown deck's tab says while the column Autopilot steps through it, empty otherwise.
    juce::String badge(uint64_t deck) const;

private:
    int badgeWidth(uint64_t deck) const;
    void sessionChanged(int) override { repaint(); }
    juce::Rectangle<int> addButton() const;
    void rename(uint64_t deck);
    // The gap between tabs nearest `x`, tabs.size() being after the last.
    size_t gapAt(int x) const;
    void moveTo(uint64_t deck, size_t gap);

    Session& session_;
    std::unique_ptr<juce::TextEditor> editor_;
    std::optional<uint64_t> pressed_;
    std::optional<size_t> dropGap_;
};

// One row of rows: each layer (top first) with its compact control and a cell per column of the shown deck.
class LayerStack : public juce::Component,
                   public juce::DragAndDropTarget,
                   public juce::FileDragAndDropTarget,
                   public PointerTips,
                   private Session::View {
public:
    explicit LayerStack(Session& session);
    ~LayerStack() override;

    // Lays out for this width; returns the height the rows need.
    int layout(int width);
    int slots() const;
    juce::Rectangle<int> columnArea(int slot) const;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;
    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void fileDragMove(const juce::StringArray&, int x, int y) override;
    void fileDragExit(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    struct Row {
        uint64_t layer = 0;
        bool group = false;
        juce::String groupName;
        juce::Rectangle<int> bounds;
    };
    const std::vector<Row>& rows() const { return rows_; }
    juce::Rectangle<int> controlArea(uint64_t layer) const;
    // The Autopilot label on the layer's control: "manual" with the bars left while a trigger holds it.
    juce::String autopilotLabel(uint64_t layer) const;
    Menu layerMenu(uint64_t layer);
    Menu cellMenu(uint64_t layer, int slot);
    // Drops `layer` onto `onto`'s control: it moves just above it, into its group or out of one.
    void moveLayerOnto(uint64_t layer, uint64_t onto);
    // Delete: clears every selected clip's cell.
    void removeSelectedClip();
    // On the selected clips, each one undo step; pasted and duplicated clips get fresh effect ids.
    void copyClips();
    // At the cell, or the selected block's top left, as the copied block lay.
    void pasteClips(std::optional<Session::ClipCell> at = {});
    void duplicateClips();
    void copyEffects(const ChainOwner& owner);
    void hoistClipEffects(uint64_t layer);
    void pasteEffects(const ChainOwner& owner);

private:
    enum class Part { None, Thumb, Label, Name, Bypass, Solo, Blend, Auto, Opacity, AddLayer };
    struct Hit {
        Part part = Part::None;
        uint64_t layer = 0;
        int slot = -1;
    };

    void sessionChanged(int changes) override;
    void rebuildRows();
    Hit partAt(juce::Point<int> point) const;
    const Row* findRow(uint64_t layer) const;
    juce::Rectangle<int> cellBounds(const Row& row, int slot) const;
    juce::Rectangle<int> controlBounds(const Row& row) const;
    std::optional<uint64_t> columnAt(int slot) const;
    const ClipRef* clipAt(const Row& row, int slot) const;
    void paintControl(juce::Graphics& g, const Row& row);
    void paintCell(juce::Graphics& g, const Row& row, int slot);
    void repaintLiveCells();
    void place(uint64_t layer, int slot, const ClipRef& clip);
    void pressCell(Session::ClipCell cell, juce::ModifierKeys mods);
    std::vector<Session::ClipCell> targets() const;
    std::vector<Session::CopiedClip> block(Session::ClipCell origin) const;
    void placeBlock(const std::vector<Session::CopiedClip>& clips, Session::ClipCell at, std::vector<Edit> first,
                    bool fresh);
    void moveBlock(Session::ClipCell from, Session::ClipCell to, bool copy);
    void setOpacity(const Row& row, int x);
    void showBlendMenu(const Row& row);
    void showAutopilotMenu(const Row& row);
    void renameLayer(uint64_t layer);
    void relinkToFile(const std::string& hash);

    Session& session_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::vector<Row> rows_;
    int width_ = 0;
    int tileWidth_ = 0;
    int rowHeight_ = 0;
    juce::Rectangle<int> addLayer_;
    Hit pressed_;
    bool dragging_ = false;
    bool collapse_ = false;
    std::optional<std::pair<uint64_t, double>> dragOpacity_;
    double lastOpacitySend_ = 0.0;
    std::optional<std::pair<uint64_t, int>> dropTarget_;
    // Cells drawn playing or queued, which a clock update repaints.
    std::set<std::pair<uint64_t, int>> liveCells_;
    std::unique_ptr<juce::TextEditor> editor_;
};

// Column names along the top of the grid: clicking one triggers the column, double-click renames it, right-click
// fires, renames or clears it. Effects dropped on the Composition header go onto the composition.
class ColumnRow : public juce::Component, public juce::DragAndDropTarget, public PointerTips, private Session::View {
public:
    ColumnRow(Session& session, LayerStack& stack);
    ~ColumnRow() override;
    void paint(juce::Graphics&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    juce::String tipAt(juce::Point<int> point) override;
    Menu columnMenu(int slot);

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;

private:
    void sessionChanged(int) override { repaint(); }
    void rename(int slot);

    Session& session_;
    LayerStack& stack_;
    bool dropOnComposition_ = false;
    std::unique_ptr<juce::TextEditor> editor_;
};

class ClipGrid : public juce::Component {
public:
    explicit ClipGrid(Session& session);
    void resized() override;
    void paint(juce::Graphics&) override;
    LayerStack& stack() { return stack_; }
    ColumnRow& columns() { return columns_; }
    DeckTabs& decks() { return decks_; }

private:
    DeckTabs decks_;
    LayerStack stack_;
    ColumnRow columns_;
    juce::Viewport viewport_;
};

} // namespace azl::ui
