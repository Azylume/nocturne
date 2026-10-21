#include "ui/app/gig_editor.hpp"

#include "document/composition_file.hpp"
#include "ui/app/look.hpp"

namespace azl::ui {

namespace {

constexpr int kRow = 22;
constexpr int kGap = 3;

void styleField(juce::TextEditor& field, const juce::String& tip)
{
    field.setFont(Look::font());
    field.setTooltip(tip);
    field.setSelectAllWhenFocused(true);
}

} // namespace

GigEditor::GigEditor(Session& session, std::filesystem::path folder) : session_(session), folder_(std::move(folder))
{
    styleField(date_, "The night's date, YYYY-MM-DD, on the venue's clock");
    styleField(window_, "When the show runs, HH:MM-HH:MM on the venue's clock; an end before the start is after "
                        "midnight");
    add_.setTooltip("Add a lineup slot");
    save_.setTooltip("Write the Gig and send its deck switches and window to the venue, as one undo step");
    close_.setTooltip("Back to the cues; what is not saved is dropped");
    add_.onClick = [this] { addSlot(); };
    save_.onClick = [this] { save(); };
    close_.onClick = [this] {
        if (onClose)
            onClose();
    };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&date_, &window_, &add_, &save_, &close_})
        addAndMakeVisible(c);
}

void GigEditor::load(std::optional<std::string> date)
{
    problem_.clear();
    const std::string venue = session_.host();
    std::optional<Gig> found;
    try {
        if (date)
            found = loadGig(gigPath(folder_, venue, *date));
        else
            found = currentGig(venueGigs(folder_, venue), unixTime());
    } catch (const std::exception&) {
    }
    gig_ = found.value_or(Gig{.venue = venue, .date = date.value_or(dateText(localDay(int64_t(unixTime()))))});
    rebuild();
}

juce::String GigEditor::deckName(uint64_t deck) const
{
    for (const Deck& d : session_.composition().decks)
        if (d.id == deck)
            return utf8(d.name);
    return "Deck " + juce::String(deck) + " (missing)";
}

void GigEditor::rebuild()
{
    date_.setText(gig_.date, false);
    window_.setText(clockText(gig_.start) + "-" + clockText(gig_.end), false);
    rows_.clear();
    for (size_t i = 0; i < gig_.lineup.size(); ++i) {
        auto row = std::make_unique<Row>();
        const GigSlot& slot = gig_.lineup[i];
        styleField(row->dj, "Who plays");
        styleField(row->start, "When the deck switches, HH:MM; on the next bar after it");
        row->dj.setText(utf8(slot.dj), false);
        row->start.setText(clockText(slot.start), false);
        row->deck.setButtonText(deckName(slot.deck));
        row->deck.setTooltip("The deck this DJ's slot shows; its first column fires");
        row->earlier.setTooltip("Five minutes earlier, saved and sent at once");
        row->later.setTooltip("Five minutes later, saved and sent at once");
        row->remove.setButtonText(juce::String::fromUTF8("\xc3\x97"));
        row->remove.setTooltip("Remove the slot");
        row->deck.onClick = [this, i, b = &row->deck] {
            Menu menu;
            for (const Deck& d : session_.composition().decks)
                menu.push_back({.label = utf8(d.name),
                                .run = [this, i, b, id = d.id] {
                                    gig_.lineup[i].deck = id;
                                    b->setButtonText(deckName(id));
                                },
                                .ticked = gig_.lineup[i].deck == d.id});
            showMenu(*b, menu);
        };
        // Later, as each rebuilds the row whose button was clicked.
        const auto later = [this](std::function<void(GigEditor&)> work) {
            return [safe = juce::Component::SafePointer<GigEditor>(this), work] {
                juce::MessageManager::callAsync([safe, work] {
                    if (safe)
                        work(*safe);
                });
            };
        };
        row->earlier.onClick = later([i](GigEditor& e) { e.shift(i, -5); });
        row->later.onClick = later([i](GigEditor& e) { e.shift(i, 5); });
        row->remove.onClick = later([i](GigEditor& e) { e.removeSlot(i); });
        for (juce::Component* c : std::initializer_list<juce::Component*>{&row->dj, &row->start, &row->deck,
                                                                          &row->earlier, &row->later, &row->remove})
            addAndMakeVisible(c);
        rows_.push_back(std::move(row));
    }
    resized();
    repaint();
}

bool GigEditor::readFields()
{
    Gig gig = gig_;
    gig.venue = session_.host();
    gig.date = date_.getText().trim().toStdString();
    const juce::String window = window_.getText().trim();
    const std::optional<int> start = clockMinutes(window.upToFirstOccurrenceOf("-", false, false).trim().toStdString());
    const std::optional<int> end = clockMinutes(window.fromFirstOccurrenceOf("-", false, false).trim().toStdString());
    if (!dateDays(gig.date) || !start || !end) {
        problem_ = "The date reads YYYY-MM-DD and the window HH:MM-HH:MM";
        repaint();
        return false;
    }
    gig.start = *start;
    gig.end = *end;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const std::optional<int> at = clockMinutes(rows_[i]->start.getText().trim().toStdString());
        if (!at) {
            problem_ = "Slot " + juce::String(int(i) + 1) + "'s start reads HH:MM";
            repaint();
            return false;
        }
        gig.lineup[i].dj = rows_[i]->dj.getText().trim().toStdString();
        gig.lineup[i].start = *at;
    }
    gig_ = gig;
    return true;
}

bool GigEditor::save()
{
    if (!readFields())
        return false;
    try {
        const Edit edit = gigEdit(gig_, session_.composition());
        saveGig(gigPath(folder_, gig_.venue, gig_.date), gig_);
        session_.client().edit(edit);
        problem_.clear();
    } catch (const std::exception& e) {
        problem_ = e.what();
    }
    rebuild();
    return problem_.isEmpty();
}

void GigEditor::shift(size_t slot, int minutes)
{
    if (!readFields() || slot >= gig_.lineup.size())
        return;
    shiftSlot(gig_, slot, minutes);
    rebuild();
    save();
}

void GigEditor::addSlot()
{
    if (!readFields())
        return;
    const int start = gig_.lineup.empty() ? gig_.start : (gig_.lineup.back().start + 60) % (24 * 60);
    gig_.lineup.push_back({"DJ " + std::to_string(gig_.lineup.size() + 1), start, session_.composition().deck});
    rebuild();
}

void GigEditor::removeSlot(size_t slot)
{
    if (!readFields() || slot >= gig_.lineup.size())
        return;
    gig_.lineup.erase(gig_.lineup.begin() + ptrdiff_t(slot));
    rebuild();
}

void GigEditor::paint(juce::Graphics& g)
{
    const Palette& p = Look::palette();
    g.fillAll(p.panel);
    g.setFont(Look::font());
    g.setColour(p.muted);
    const auto area = getLocalBounds().reduced(6, 4);
    drawText(g, "Gig", area.withHeight(kRow), juce::Justification::centredLeft);
    if (rows_.empty())
        drawText(g, "No lineup yet: add a slot per DJ.", area.withY(area.getY() + kRow + kGap).withHeight(kRow),
                 juce::Justification::centredLeft);
    if (problem_.isNotEmpty()) {
        g.setColour(p.fault);
        drawText(g, problem_, area.withTop(area.getBottom() - kRow).reduced(70, 0), juce::Justification::centredLeft);
    }
}

void GigEditor::resized()
{
    auto area = getLocalBounds().reduced(6, 4);
    auto top = area.removeFromTop(kRow);
    top.removeFromLeft(32);
    date_.setBounds(top.removeFromLeft(96));
    top.removeFromLeft(kGap);
    window_.setBounds(top.removeFromLeft(100));
    close_.setBounds(top.removeFromRight(56));
    auto bottom = area.removeFromBottom(kRow);
    add_.setBounds(bottom.removeFromLeft(64));
    save_.setBounds(bottom.removeFromRight(64));
    area.removeFromTop(kGap);
    for (const auto& row : rows_) {
        auto line = area.removeFromTop(kRow);
        area.removeFromTop(kGap);
        row->remove.setBounds(line.removeFromRight(22));
        row->later.setBounds(line.removeFromRight(30));
        row->earlier.setBounds(line.removeFromRight(30));
        line.removeFromRight(kGap);
        row->deck.setBounds(line.removeFromRight(110));
        line.removeFromRight(kGap);
        row->start.setBounds(line.removeFromRight(52));
        line.removeFromRight(kGap);
        row->dj.setBounds(line);
    }
}

} // namespace azl::ui
