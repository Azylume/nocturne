// Paths outside ASCII on Windows. The engine gets every path as a UTF-8 string (command line, documents, the
// protocol) and makes a std::filesystem::path of it, which reads the string in the process code page: the
// application manifest makes that UTF-8.
#include "test_files.hpp"

#include "document/composition_file.hpp"
#include "media/video_decoder.hpp"

#include <doctest.h>

#include <string>

namespace {

std::string utf8(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace

TEST_CASE("windows: a composition saves and loads and a clip decodes under a folder named \"Őszi ünnep テスト\"")
{
    TempDir dir;
    const fs::path folder = dir.path / u8"Őszi ünnep テスト";
    fs::create_directory(folder);
    fs::copy_file(mediaDir() / "count_709_tv.mp4", folder / u8"őszi.mp4");
    const std::string at = utf8(folder);

    azl::Composition composition = azl::defaultComposition();
    composition.bpm = 133;
    composition.screens.push_back({.id = 1, .name = "Főképernyő", .slices = {{.id = 1}}});
    azl::saveCompositionFile(fs::path(at + "\\show.json"), {composition});
    CHECK(fs::exists(folder / "show.json"));
    const auto loaded = azl::loadCompositionFile(fs::path(at + "\\show.json"));
    REQUIRE(loaded);
    CHECK(loaded->composition.bpm == 133);
    CHECK(loaded->composition.screens.back().name == "Főképernyő");

    azl::VideoDecoder decoder(nullptr, at + "\\őszi.mp4", {});
    CHECK(decoder.waitFrame(0, true));
}
