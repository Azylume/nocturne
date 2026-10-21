#include "document/composition_file.hpp"
#include "import/resolume_import.hpp"
#include "media/composition_media.hpp"

#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

fs::path fixture(const std::string& name)
{
    const char* dir = std::getenv("NOCTURNE_GOLDEN_DIR");
    return fs::path(dir ? dir : NOCTURNE_GOLDEN_DIR) / "import" / name;
}

std::string read(const fs::path& path)
{
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// The media of the trimmed fixture as transcodes and links would name them, all but closer_moon1.
fs::path mediaFolder()
{
    const fs::path dir = fs::temp_directory_path() / ("nocturne-import-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    for (const char* name : {"opener_sun1.mp4", "opener_sun2.mp4", "loop-01.mp4", "loop-02.mp4", "still_logo.mp4",
                             "text_title.mp4"})
        std::ofstream(dir / name) << name;
    return dir;
}

bool has(const std::vector<azl::ImportNote>& notes, const std::string& where, const std::string& what,
         const std::string& also = {})
{
    for (const azl::ImportNote& n : notes)
        if (n.where == where && n.what.find(what) != std::string::npos && n.what.find(also) != std::string::npos)
            return true;
    return false;
}

struct Trimmed {
    azl::ImportReport report;
    azl::ImportedComposition imported;
    fs::path media;
};

Trimmed importTrimmed(const azl::AvcOptions& options = {})
{
    Trimmed t;
    t.media = mediaFolder();
    t.imported =
        azl::importAvc(*azl::parseXmlFile(fixture("trimmed.avc").string()), azl::mediaIn(t.media), t.report, options);
    return t;
}

std::vector<std::string> types(const std::vector<azl::Effect>& effects)
{
    std::vector<std::string> out;
    for (const azl::Effect& e : effects)
        out.push_back(e.type);
    return out;
}

} // namespace

TEST_CASE("a trimmed Resolume composition imports to the expected snapshot")
{
    Trimmed t = importTrimmed();
    nlohmann::json file = azl::portableFile(t.imported, t.media);
    fs::remove_all(t.media);
    CHECK(file["mediaRoot"] == t.media.lexically_normal().string());
    file.erase("mediaRoot");
    const fs::path expectedPath = fixture("trimmed.expected.json");
    if (std::getenv("NOCTURNE_UPDATE_GOLDEN"))
        std::ofstream(expectedPath) << file.dump(2) << "\n";
    const nlohmann::json expected = nlohmann::json::parse(read(expectedPath));
    CHECK_MESSAGE(file == expected, file.dump(2));

    const azl::CompositionFile loaded = azl::compositionFileFromJson(file);
    CHECK(nlohmann::json(loaded.composition) == nlohmann::json(t.imported.composition));
    azl::Composition checked = loaded.composition;
    std::vector<std::string> warnings;
    azl::dropUnknownEffects(checked, warnings);
    CHECK_MESSAGE(warnings.empty(), warnings.front());
}

TEST_CASE("clip effects land on their clips, each clip with its own settings")
{
    Trimmed t = importTrimmed({.hoistClipChains = false});
    fs::remove_all(t.media);
    const azl::Composition& c = t.imported.composition;
    const azl::Layer& base = c.layers[0];
    CHECK(base.effects.empty());
    const azl::ClipRef& sun = base.clips.at(1);
    REQUIRE(sun.effects.size() == 1);
    CHECK(sun.effects[0].type == "colorize");
    const azl::ClipRef& moon = base.clips.at(3);
    REQUIRE(moon.effects.size() == 1);
    CHECK(moon.effects[0].params.at("hue") == doctest::Approx(0.0956).epsilon(0.001));
    const azl::Layer& text = c.layers[2];
    CHECK(text.clips.at(1).effects.empty());
    CHECK(text.clips.at(2).effects.empty());
}

TEST_CASE("a chain all clips of a layer, or of a layer in one deck, carry alike runs as the layer's or the deck's "
          "layer chain, unless kept on the clips")
{
    using Types = std::vector<std::string>;
    Trimmed hoisted = importTrimmed();
    fs::remove_all(hoisted.media);
    const azl::Layer& low = hoisted.imported.composition.layers[1];
    CHECK(types(low.effects) == Types{"edgeDetection", "trails"});
    for (const auto& [column, clip] : low.clips)
        CHECK(clip.effects.empty());
    // Within one deck: base's clips differ between its decks but agree within each, so each deck runs its own.
    const azl::Layer& base = hoisted.imported.composition.layers[0];
    CHECK(base.effects.empty());
    REQUIRE(base.decks.size() == 2);
    CHECK(types(*base.decks.at(1).effects) == Types{"colorize"});
    CHECK(base.decks.at(2).effects->at(0).params != base.decks.at(1).effects->at(0).params);
    for (const auto& [column, clip] : base.clips)
        CHECK(clip.effects.empty());
    const azl::Layer& text = hoisted.imported.composition.layers[2];
    CHECK(text.decks.empty());
    CHECK(text.clips.at(1).effects.empty());

    Trimmed kept = importTrimmed({.hoistClipChains = false});
    fs::remove_all(kept.media);
    const azl::Layer& keptLow = kept.imported.composition.layers[1];
    CHECK(types(keptLow.effects) == Types{"edgeDetection", "trails"});
    for (const auto& [column, clip] : keptLow.clips)
        CHECK(clip.effects.empty());
}

TEST_CASE("the import report lists every unsupported element with where it was")
{
    Trimmed t = importTrimmed();
    fs::remove_all(t.media);
    const auto& no = t.report.unsupported;
    CHECK(has(no, "layer base / effect Levels", "no Nocturne effect"));
    CHECK(has(no, "deck opener / column 1 / layer low / clip loop-01 / effect Bloom", "no Nocturne effect"));
    CHECK(has(no, "layer text / effect TwitchEffect", "no Nocturne effect (it was bypassed)"));
    CHECK(has(no, "layer text / effect Stripper", "blend Add not imported"));
    CHECK(has(no, "deck opener / column 2 / layer low / clip loop-02", "parameter Speed = 0.5"));
    CHECK(has(no, "media", "/Volumes/show/loops/closer_moon1.mov not found"));
    CHECK(has(t.report.approximated, "layer low", "audio modulator 'bass'"));
    CHECK(has(t.report.approximated, "layer base / clip effect Colorize", "colour value 0.89 not kept", "(2 clips)"));
    CHECK(has(t.report.approximated, "layer base / clip effect Colorize", "colour value 0.941 not kept", "(1 clip)"));
    for (const azl::ImportNote& n : no)
        CHECK_MESSAGE(n.what.find("clip effect") == std::string::npos, n.where << ": " << n.what);

    const std::string text = azl::formatReport(t.report, "Import of trimmed.avc");
    CHECK(text.find("## Not imported (" + std::to_string(no.size()) + ")") != std::string::npos);
    CHECK(text.find("- layer base / effect Levels: no Nocturne effect") != std::string::npos);
}

TEST_CASE("the five-screen preset imports to five full-frame screens")
{
    azl::ImportReport report;
    const std::vector<std::string> displays{"window-1", "window-2", "window-3", "window-4", "window-5"};
    const std::vector<azl::Screen> screens =
        azl::importAdvancedOutput(*azl::parseXmlFile(fixture("five-screens.xml").string()), displays, report);
    REQUIRE(screens.size() == 5);
    const std::vector<std::string> names{"Inner left", "Inner mid", "Inner right", "Outer A ", "Outer B"};
    for (size_t i = 0; i < screens.size(); ++i) {
        CAPTURE(i);
        CHECK(screens[i].name == names[i]);
        CHECK(screens[i].display == displays[i]);
        REQUIRE(screens[i].slices.size() == 1);
        const azl::Slice& s = screens[i].slices[0];
        CHECK(s.id == i + 1);
        CHECK(s.input == 0);
        CHECK(s.inputRect == std::array<double, 4>{0.0, 0.0, 1.0, 1.0});
        CHECK(s.corners == std::array<double, 8>{0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0});
    }
    CHECK_MESSAGE(report.unsupported.empty(), report.unsupported.front().what);

    azl::ImportReport fallback;
    const std::vector<azl::Screen> own =
        azl::importAdvancedOutput(*azl::parseXmlFile(fixture("five-screens.xml").string()), {}, fallback);
    CHECK(own[0].display == "VirtualInner left");
}
