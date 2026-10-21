#include "ui/app/show_browser.hpp"

#include "ui/app/look.hpp"

#include <filesystem>

namespace azl::ui {

namespace {

std::string folderOf(const std::string& path)
{
    return std::filesystem::path(path).parent_path().string();
}

} // namespace

ShowBrowser::ShowBrowser(Session& session, bool saving)
    : session_(session), saving_(saving), action_(saving ? "Save as" : "Open")
{
    title_.setText(saving ? "Save the show as" : "Open a show", juce::dontSendNotification);
    title_.setFont(Look::font(true));
    question_.setColour(juce::Label::textColourId, Look::palette().accent);
    search_.setTextToShowWhenEmpty("Search", Look::palette().muted);
    search_.onTextChange = [this] { filter(); };
    path_.setTextToShowWhenEmpty(saving ? "Name" : "Or a path on the engine machine", Look::palette().muted);
    path_.onReturnKey = [this] { act(); };
    for (juce::TextEditor* field : {&search_, &path_})
        field->onEscapeKey = [this] { close(); };
    if (saving)
        path_.setText(juce::File(utf8(session.client().savedPath())).getFileNameWithoutExtension());
    list_.setModel(this);
    list_.setRowHeight(36);
    list_.setColour(juce::ListBox::backgroundColourId, Look::palette().field);
    search_.setTooltip("Show only the shows whose names contain this");
    path_.setTooltip(saving ? "The name to save the show under, in the folder chosen"
                            : "Open the show at this path on the engine machine");
    folder_.setTooltip("The folder on the engine machine to save into");
    list_.setTooltip("The shows on the engine machine");
    action_.setTooltip(saving ? "Save the show on the engine machine" : "Open the chosen show");
    cancel_.setTooltip("Close without saving or opening");
    action_.getProperties().set("accent", true);
    action_.onClick = [this] { act(); };
    cancel_.onClick = [this] { close(); };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&title_, &search_, &list_, &path_, &action_, &cancel_})
        addAndMakeVisible(c);
    addChildComponent(question_);
    addChildComponent(folder_);
    folder_.setVisible(saving);
    session_.addView(this);
    session_.client().requestShows();
    sessionChanged(Session::Shows);
}

ShowBrowser::~ShowBrowser()
{
    session_.removeView(this);
}

void ShowBrowser::sessionChanged(int changes)
{
    if (!(changes & Session::Shows))
        return;
    shows_ = session_.client().shows();
    const std::string chosen = folder_.getSelectedId() > 0 ? folders_[size_t(folder_.getSelectedId() - 1)] : "";
    folders_.clear();
    const std::string saved = session_.client().savedPath();
    for (const std::string& folder : {saved.empty() ? std::string() : folderOf(saved), shows_.showsFolder})
        if (!folder.empty() && std::find(folders_.begin(), folders_.end(), folder) == folders_.end())
            folders_.push_back(folder);
    for (const ShowInfo& show : shows_.shows)
        if (std::find(folders_.begin(), folders_.end(), folderOf(show.path)) == folders_.end())
            folders_.push_back(folderOf(show.path));
    folder_.clear(juce::dontSendNotification);
    for (size_t i = 0; i < folders_.size(); ++i)
        folder_.addItem(utf8(folders_[i]), int(i) + 1);
    const auto keep = std::find(folders_.begin(), folders_.end(), chosen);
    folder_.setSelectedId(keep == folders_.end() ? 1 : int(keep - folders_.begin()) + 1, juce::dontSendNotification);
    filter();
}

void ShowBrowser::filter()
{
    const juce::String text = search_.getText().trim();
    rows_.clear();
    for (const ShowInfo& show : shows_.shows)
        if (text.isEmpty() || utf8(show.path).containsIgnoreCase(text))
            rows_.push_back(show);
    list_.updateContent();
    list_.repaint();
}

void ShowBrowser::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= int(rows_.size()))
        return;
    const Palette& p = Look::palette();
    const ShowInfo& show = rows_[size_t(row)];
    g.fillAll(selected ? p.buttonHover : p.field);
    auto area = juce::Rectangle<int>(width, height).reduced(8, 3);
    auto top = area.removeFromTop(height / 2 - 3);
    const juce::String when = juce::Time(int64_t(show.modified * 1000.0)).formatted("%Y-%m-%d %H:%M") + "   " +
                              juce::File::descriptionOfSizeInBytes(int64_t(show.size));
    g.setFont(Look::font());
    g.setColour(p.muted);
    drawText(g, when, top, juce::Justification::centredRight);
    g.setColour(p.white);
    g.setFont(Look::font(true));
    drawText(g, utf8(show.name), top.withTrimmedRight(170), juce::Justification::centredLeft, true);
    g.setFont(Look::font());
    if (!show.mediaPresent) {
        g.setColour(p.fault);
        drawText(g, "media missing", area, juce::Justification::centredRight);
        area.removeFromRight(100);
    }
    g.setColour(p.muted);
    drawText(g, utf8(folderOf(show.path)), area, juce::Justification::centredLeft, true);
}

void ShowBrowser::take(int row)
{
    if (row < 0 || row >= int(rows_.size()))
        return;
    const ShowInfo& show = rows_[size_t(row)];
    if (!saving_)
        return path_.setText(utf8(show.path));
    path_.setText(utf8(show.name));
    const auto folder = std::find(folders_.begin(), folders_.end(), folderOf(show.path));
    if (folder != folders_.end())
        folder_.setSelectedId(int(folder - folders_.begin()) + 1, juce::dontSendNotification);
}

void ShowBrowser::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    take(row);
    act();
}

juce::String ShowBrowser::getTooltipForRow(int)
{
    return saving_ ? "Click to take its name, double-click to save over it"
                   : "Click to choose it, double-click to open it";
}

void ShowBrowser::act()
{
    if (confirmed_) {
        const auto then = std::move(confirmed_);
        close();
        return then();
    }
    juce::String text = path_.getText().trim();
    if (text.isEmpty())
        return;
    EngineClient& client = session_.client();
    if (!saving_) {
        const std::string path = text.toStdString();
        if (!client.unsaved()) {
            close();
            return client.loadComposition(path);
        }
        return ask("The live show has unsaved changes. Opening replaces it; the engine keeps a backup of it.",
                   [&client, path] { client.loadComposition(path); });
    }
    if (folder_.getSelectedId() <= 0)
        return;
    if (!text.endsWithIgnoreCase(".json"))
        text += ".json";
    const std::string path = (std::filesystem::path(folders_[size_t(folder_.getSelectedId() - 1)]) / text.toStdString()).string();
    for (const ShowInfo& show : shows_.shows)
        if (show.path == path)
            return ask(utf8(show.name) + " exists. Saving replaces it.", [&client, path] { client.saveComposition(path); });
    close();
    client.saveComposition(path);
}

void ShowBrowser::ask(const juce::String& question, std::function<void()> then)
{
    confirmed_ = std::move(then);
    question_.setText(question, juce::dontSendNotification);
    question_.setVisible(true);
    action_.setButtonText("Replace");
}

void ShowBrowser::close()
{
    confirmed_ = nullptr;
    setVisible(false);
}

juce::Rectangle<int> ShowBrowser::panel() const
{
    return getLocalBounds().withSizeKeepingCentre(std::min(getWidth() - 40, 680), std::min(getHeight() - 40, 480));
}

void ShowBrowser::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.55f));
    g.setColour(Look::palette().panel);
    g.fillRect(panel());
}

void ShowBrowser::resized()
{
    auto area = panel().reduced(12);
    title_.setBounds(area.removeFromTop(24));
    area.removeFromTop(4);
    search_.setBounds(area.removeFromTop(24));
    area.removeFromTop(6);
    auto buttons = area.removeFromBottom(28);
    action_.setBounds(buttons.removeFromRight(90));
    buttons.removeFromRight(6);
    cancel_.setBounds(buttons.removeFromRight(80));
    area.removeFromBottom(6);
    question_.setBounds(area.removeFromBottom(24));
    auto fields = area.removeFromBottom(24);
    if (saving_) {
        folder_.setBounds(fields.removeFromLeft(fields.getWidth() * 3 / 5));
        fields.removeFromLeft(6);
    }
    path_.setBounds(fields);
    area.removeFromBottom(6);
    list_.setBounds(area);
}

} // namespace azl::ui
