#include "output/outputs.hpp"

#include <algorithm>
#include <set>

namespace azl {

namespace {

std::unique_ptr<DisplayOutput> take(std::vector<std::unique_ptr<DisplayOutput>>& outputs, const std::string& display)
{
    auto it = std::find_if(outputs.begin(), outputs.end(), [&](const auto& o) { return o->display() == display; });
    if (it == outputs.end())
        return nullptr;
    std::unique_ptr<DisplayOutput> output = std::move(*it);
    outputs.erase(it);
    return output;
}

std::set<std::string> attachedOf(const std::vector<std::unique_ptr<DisplayOutput>>& outputs)
{
    std::set<std::string> attached;
    for (const auto& output : outputs)
        if (output->attached())
            attached.insert(output->display());
    return attached;
}

std::string mainOf(const std::vector<std::unique_ptr<DisplayOutput>>& outputs)
{
    for (const auto& output : outputs)
        if (output->main())
            return output->display();
    return {};
}

} // namespace

Outputs::Outputs(Engine& engine, Displays& displays, std::string mainDisplay, KnownDisplays* known)
    : engine_(engine), displays_(displays), mainDisplay_(std::move(mainDisplay)), known_(known)
{
    displays_.onChange([this] {
        update();
        if (displaysChanged_)
            displaysChanged_();
    });
}

Outputs::~Outputs()
{
    displays_.onChange(nullptr);
}

void Outputs::setComposition(std::shared_ptr<const Composition> composition)
{
    composition_ = std::move(composition);
    update();
}

void Outputs::setMainDisplay(std::string display)
{
    mainDisplay_ = std::move(display);
    chooseMain();
}

void Outputs::setDelay(const std::string& display, int ms)
{
    delays_[display] = ms;
    if (DisplayOutput* output = find(display))
        output->setDelay(ms);
}

void Outputs::setGuides(bool shown)
{
    guides_ = shown;
    update();
}

void Outputs::setTarget(const std::string& display, std::optional<Point> point)
{
    if (point)
        targets_[display] = *point;
    else
        targets_.erase(display);
    update();
}

std::optional<Point> Outputs::target(const std::string& display) const
{
    auto it = targets_.find(display);
    if (it == targets_.end())
        return std::nullopt;
    return it->second;
}

void Outputs::identify(const std::string& display, double seconds)
{
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                           std::chrono::duration<double>(std::max(seconds, 0.0)));
    if (display.empty())
        identify_.clear();
    identify_[display] = until;
    update();
}

ScreenView Outputs::view(const Composition& composition, const Screen& screen) const
{
    std::chrono::steady_clock::time_point until{};
    for (const std::string& key : {std::string(), screen.display})
        if (auto it = identify_.find(key); it != identify_.end())
            until = std::max(until, it->second);
    return screenView(composition, screen, guides_, target(screen.display), until);
}

std::vector<DisplayOutput*> Outputs::all() const
{
    std::vector<DisplayOutput*> all;
    for (const auto& output : outputs_)
        all.push_back(output.get());
    return all;
}

DisplayOutput* Outputs::find(const std::string& display) const
{
    for (const auto& output : outputs_)
        if (output->display() == display)
            return output.get();
    return nullptr;
}

// Outputs whose screen is gone close; the others follow their screen and whether their display is attached.
void Outputs::update()
{
    const std::vector<DisplayInfo> displays = displays_.attached();
    if (known_)
        known_->add(displays);
    const std::set<std::string> wasAttached = attachedOf(outputs_);
    const std::string wasMain = mainOf(outputs_);
    std::set<std::string> attached;
    for (const DisplayInfo& display : displays)
        if (display.mirrorOf.empty())
            attached.insert(display.id);

    std::vector<std::unique_ptr<DisplayOutput>> next;
    for (const Screen& screen : composition_->screens) {
        if (screen.display.empty())
            continue;
        std::unique_ptr<DisplayOutput> output = take(outputs_, screen.display);
        if (!output) {
            output = std::make_unique<DisplayOutput>(engine_, screen.display, vsync_);
            output->setDelay(delays_[screen.display]);
        }
        output->setScreen(std::make_shared<const ScreenView>(view(*composition_, screen)));
        const bool present = attached.contains(screen.display);
        if (present && !output->attached()) {
            if (auto surface = displays_.open(screen.display, engine_.device()))
                output->attach(std::move(surface));
        } else if (!present && output->attached()) {
            output->detach();
        }
        next.push_back(std::move(output));
    }
    outputs_.swap(next);
    chooseMain(attachedOf(outputs_) == wasAttached ? wasMain : std::string());
}

// Without the chosen main display attached, `keep` stays main while attached (a reordering of the screens), else the
// first attached output.
void Outputs::chooseMain(const std::string& keep)
{
    DisplayOutput* main = find(mainDisplay_);
    if (!main || !main->attached())
        main = find(keep);
    if (!main || !main->attached()) {
        auto it = std::find_if(outputs_.begin(), outputs_.end(), [](const auto& o) { return o->attached(); });
        main = it == outputs_.end() ? nullptr : it->get();
    }
    for (const auto& output : outputs_)
        output->setMain(output.get() == main);
}

} // namespace azl
