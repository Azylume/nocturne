#pragma once

#include "ui/app/map_geometry.hpp"
#include "ui/app/widgets.hpp"

namespace azl::ui {

// What only this UI knows of the Map tab: the selection, the screen opened large, the view and the overlay switches.
struct MapState {
    uint64_t screen = 0;
    // A slice, or with `mask` a mask of the screen (or of the slice `maskSlice`); 0 for none.
    uint64_t item = 0;
    bool mask = false;
    uint64_t maskSlice = 0;
    // The selected corner, warp grid point or mask point; -1 for none.
    int point = -1;
    uint64_t focus = 0;
    bool input = false;
    bool targets = true;
    bool guides = true;
    // Selection and navigation only: no handles, nudges or edits of the mapping.
    bool locked = false;
};

class MapView;

// Output view: every screen as a frame with its live picture, its slices and masks, and handles on the selection.
// Input view: the selected slice's input large, with each slice of that input as a rectangle. The wheel zooms around
// the cursor and a drag on empty space pans; neither ever edits.
class MapCanvas : public juce::Component {
public:
    MapCanvas(Session& session, MapView& map);

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    void fit();
    // The view's scale as a percentage of the fitted one.
    int zoomPercent() const;
    Viewport viewport() const;
    // Where a point of a screen (its 0..1 space) or of the input (0..1) shows on the canvas.
    juce::Point<int> screenPixel(uint64_t screen, Point unit) const;
    juce::Point<int> inputPixel(Point unit) const;
    juce::Point<int> rotatePixel() const;
    juce::Point<int> titlePixel(uint64_t screen) const;
    void documentChanged();
    // The screens on the canvas: all but the hidden ones, or the one opened large.
    std::vector<const Screen*> shownScreens() const;

private:
    enum class Grab { Pan, Corner, Edge, Move, Rotate, InputMove, InputResize, InputNew, Frame };
    struct Drag {
        Grab grab = Grab::Pan;
        int index = -1;
        uint64_t screen = 0;
        Points base;
        std::array<double, 4> rect{};
        Point start{};
        juce::Point<float> startPixel;
        Viewport startView;
        bool moved = false;
    };

    BoardRect frameOf(uint64_t screen) const;
    BoardRect board() const;
    bool onTitle(uint64_t screen, juce::Point<float> pixel) const;
    std::optional<Drag> grabTitle(const juce::MouseEvent& e);
    Point unitAt(const BoardRect& frame, juce::Point<float> pixel) const;
    juce::Point<float> pixelAt(const BoardRect& frame, Point unit) const;
    // A slice's or mask's outline on its screen; a slice mask's `owner` is its slice.
    Points outlineOf(const Screen& screen, uint64_t item, bool mask, uint64_t owner) const;
    std::optional<Drag> grabOutput(const juce::MouseEvent& e);
    std::optional<Drag> grabInput(const juce::MouseEvent& e);
    void dragOutput(const juce::MouseEvent& e);
    void dragInput(const juce::MouseEvent& e);
    void send(bool force);
    void paintOutput(juce::Graphics& g);
    void paintInput(juce::Graphics& g);
    void paintHandles(juce::Graphics& g, const Screen& screen, const BoardRect& frame);
    InputSnap snapFor(uint64_t except, bool snap) const;

    Session& session_;
    MapView& map_;
    std::optional<Viewport> user_;
    std::optional<Drag> drag_;
    std::optional<Points> livePoints_;
    std::optional<std::array<double, 4>> liveRect_;
    std::optional<std::array<double, 4>> newRect_;
    // A frame dragged by its title: the screen and its top-left on the board.
    std::optional<std::pair<uint64_t, Point>> liveFrame_;
    double lastSend_ = 0.0;
};

// The selected screen's settings, the guides, and the selected slice's or mask's fields. Every number drags, scrolls
// or types; the relative Move, Scale and Rotate fields go back to neutral after each change.
class MapInspector : public juce::Component, private juce::AsyncUpdater {
public:
    MapInspector(Session& session, MapView& map);
    void refresh();
    void paint(juce::Graphics&) override;
    void resized() override;
    // The number field in the row labelled `label`, the `index`th of it.
    NumberField* field(const juce::String& label, int index = 0) const;
    juce::Button* button(const juce::String& text) const;
    juce::ComboBox* choiceFor(const juce::String& label) const;

private:
    struct Row {
        juce::String label;
        std::vector<juce::Component*> parts;
        bool header = false;
    };
    std::string structure() const;
    void rebuild();
    void handleAsyncUpdate() override { rebuild(); }
    void header(const juce::String& text);
    void row(const juce::String& label, std::vector<juce::Component*> parts);
    NumberField* number(double min, double max, double step, std::function<double()> get,
                        std::function<void(double)> set, const juce::String& suffix = {});
    // A field that applies `apply(base, value)` to the selected shape's points as they were when it started.
    NumberField* relative(double neutral, double min, double max, double step,
                          std::function<Points(const Points&, double)> apply, const juce::String& suffix);
    juce::TextButton* action(const juce::String& text, std::function<void()> click,
                             std::function<bool()> on = nullptr);
    juce::ComboBox* choice(std::vector<std::pair<juce::String, std::string>> items, std::function<std::string()> get,
                           std::function<void(const std::string&)> set);
    // The name as a button; a click edits it in place.
    void addNameRow(const std::string& kind, uint64_t id, const std::string& name);
    void addScreenRows(const Screen& screen);
    void addGuideRows();
    void addSliceRows(const Slice& slice);
    void addWarpRows(const Slice& slice);
    void addPointStepper();
    void addMaskRows(const Mask& mask);
    void addTransformRows();

    Session& session_;
    MapView& map_;
    std::string structure_;
    std::vector<Row> rows_;
    std::vector<std::unique_ptr<juce::Component>> owned_;
    std::vector<std::function<void()>> updaters_;
    std::unique_ptr<juce::TextEditor> renaming_;
};

// The Map tab: the tree of screens, slices and masks on the left, the toolbar and canvas, the inspector on the right.
// While it shows, the outputs draw the guides, and a corner target through the selected point.
class MapView : public juce::Component, private Session::View {
public:
    explicit MapView(Session& session);
    ~MapView() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    // A screen's row drags to another place in the list; its eye shows or hides it on the canvas.
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    // F2 or Enter renames the selected screen.
    bool keyPressed(const juce::KeyPress&) override;
    void visibilityChanged() override;
    // Esc: back to all screens. False when there was nothing to go back from.
    bool escape();

    MapState& state() { return state_; }
    MapCanvas& canvas() { return canvas_; }
    MapInspector& inspector() { return inspector_; }
    juce::Button* toolbarButton(const juce::String& text) const;
    // The tree's rows, scrolled, and where they show.
    std::vector<juce::Rectangle<int>> treeRowAreas() const;
    juce::Rectangle<int> treeArea() const;
    // The show/hide toggle on a screen's tree row.
    static juce::Rectangle<int> eyeArea(juce::Rectangle<int> row);

    const Screen* screen() const;
    const Slice* slice() const;
    const Mask* mask() const;
    // The slice holding the selected mask; null for a screen mask.
    const Slice* maskOwner() const;
    // The selected slice's or mask's handles on its screen: a slice's corners or, when warped, its grid points; a
    // mask's points (a slice mask's through its slice).
    Points shape() const;
    // The shape has edges to drag: corners or a mask, not a warp grid.
    bool hasEdges() const;
    bool cornerWarp() const;
    // The shape with point `index` moved; a corners-warp corner takes its two handles along.
    Points movedPoint(const Points& shape, int index, Point delta) const;
    // The four points the rotate handle and the frame of the shape follow.
    Points frameOf(const Points& shape) const;
    // The output's size in pixels: its display's, or 1920x1080 while the display is unknown.
    std::pair<double, double> pixelSize(const Screen& screen) const;
    // The picture the Input view shows: the selected slice's input.
    uint64_t input() const;

    void select(uint64_t screen, uint64_t item, bool mask);
    void setFocus(uint64_t screen);
    void setInputView(bool input);
    // Repaints and refreshes after a change of the UI's state.
    void changed();
    // While a drag moves the selected point, the corner target follows the hand rather than the document.
    void setLiveTarget(std::optional<Point> point);
    void updateOverlays();

    // Edits of the selection; each is one undo step unless sent inside a gesture.
    void sendShape(const Points& points);
    // Off (0), the corners warp (1) or an n x n interpolating grid, sampled from the slice's mapping; turning it off
    // keeps the outer corners.
    void setWarp(int n);
    void flattenWarp();
    // Arrow keys: the selected point, or the whole shape, by output pixels.
    bool nudge(int dx, int dy);
    void renameScreen(uint64_t screen, const std::string& name);
    // A text field over the screen's tree row; Enter renames it.
    void startRename(uint64_t screen);
    void sendInputRect(const std::array<double, 4>& rect);
    void addScreen();
    void addSlice(std::optional<std::array<double, 4>> inputRect = std::nullopt);
    void addMask(bool onSlice = false);
    void addGuide(bool vertical);
    void toggleTestCard(uint64_t screen);
    void setHidden(uint64_t screen, bool hidden);
    void moveScreen(uint64_t screen, size_t index);
    void autoSoftEdge();
    void removeSelected();

private:
    struct TreeRow {
        juce::Rectangle<int> area;
        uint64_t screen = 0;
        uint64_t item = 0;
        bool mask = false;
        uint64_t owner = 0;
    };
    void sessionChanged(int changes) override;
    void validate();
    std::vector<TreeRow> treeRows() const;
    std::optional<TreeRow> treeRowAt(juce::Point<int> point) const;
    // The list position a screen dropped at `point` takes.
    std::optional<size_t> dropIndexAt(juce::Point<int> point) const;
    void layoutToolbar();
    std::optional<Point> targetPoint() const;

    // An item added here, selected as soon as the engine has it.
    struct Pending {
        int kind = 0;
        uint64_t screen = 0;
        uint64_t slice = 0;
        size_t count = 0;
    };

    Session& session_;
    int treeScroll_ = 0;
    std::optional<Pending> pending_;
    struct ListDrag {
        uint64_t screen = 0;
        juce::Point<int> start;
        std::optional<size_t> target;
    };
    std::optional<ListDrag> listDrag_;
    MapState state_;
    MapCanvas canvas_;
    MapInspector inspector_;
    std::vector<std::unique_ptr<juce::TextButton>> toolbar_;
    juce::TextButton addScreen_{"+ Screen"};
    std::unique_ptr<juce::TextEditor> renaming_;
    std::optional<Point> liveTarget_;
    std::optional<bool> guidesShown_;
    std::string targetDisplay_;
    std::optional<Point> targetSent_;
};

} // namespace azl::ui
