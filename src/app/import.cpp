#include "document/composition_file.hpp"
#include "import/resolume_import.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kUsage = R"(usage: nocturne-import COMPOSITION.avc OUT.json [options]
       nocturne-import PRESET.xml|DIR... OUT_DIR [--displays ID,...]

Converts a Resolume Arena composition into a portable Nocturne composition file and writes a report of everything it
could not convert to OUT.report.md (nothing is dropped silently).

The second form imports Advanced Output presets or AdvancedOutput.xml files (each .xml of a DIR) on their own: each
becomes OUT_DIR/NAME.json, a composition file holding only its screens, with OUT_DIR/NAME.report.md beside it.

  --mapping FILE.xml     a Resolume Advanced Output preset or AdvancedOutput.xml: its screens and slices
  --media DIR            where the clips' media are (tools/transcode-media.sh): found by file name, else stem + .mp4 or .mov;
                         also the composition file's media root (default: the folder of OUT.json)
  --displays ID,ID,...   displays for the mapping's screens, in order (default: the source's output device ids, which
                         the engine moves to free attached displays)
  --start-column N       column of the shown deck every layer plays on load (default 1; 0 plays nothing)
  --keep-clip-chains     keep a chain every clip of a layer carries alike on each clip (default: it runs as the
                         layer's, one compile instead of one per clip)
  --report FILE          where the report goes
)";

[[noreturn]] void usage(int code)
{
    std::fputs(kUsage, code ? stderr : stdout);
    std::exit(code);
}

std::vector<std::string> split(const std::string& list)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t end = list.find(',', start);
        parts.push_back(list.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return parts;
}

bool isMapping(const fs::path& path)
{
    return fs::is_directory(path) || path.extension() == ".xml";
}

std::vector<fs::path> presets(const std::vector<std::string>& sources)
{
    std::vector<fs::path> files;
    for (const fs::path source : sources) {
        if (!fs::is_directory(source)) {
            files.push_back(source);
            continue;
        }
        std::vector<fs::path> found;
        for (const fs::directory_entry& e : fs::directory_iterator(source))
            if (e.path().extension() == ".xml")
                found.push_back(e.path());
        std::sort(found.begin(), found.end());
        files.insert(files.end(), found.begin(), found.end());
    }
    return files;
}

int importMappings(const std::vector<std::string>& sources, const fs::path& outDir,
                   const std::vector<std::string>& displays)
{
    fs::create_directories(outDir);
    int failed = 0;
    for (const fs::path& file : presets(sources)) {
        const fs::path out = outDir / (file.stem().string() + ".json");
        const fs::path reportPath = outDir / (file.stem().string() + ".report.md");
        try {
            azl::ImportReport report;
            azl::Composition composition;
            composition.screens = azl::importAdvancedOutput(*azl::parseXmlFile(file.string()), displays, report);
            azl::writeFileAtomically(out, azl::toJson(azl::CompositionFile{composition}).dump(2) + "\n");
            azl::writeFileAtomically(reportPath, azl::formatReport(report, "Import of " + file.filename().string()));
            std::printf("%s: %zu screen(s), %zu imported, %zu approximated, %zu not imported; report: %s\n",
                        file.filename().c_str(), composition.screens.size(), report.mapped.size(),
                        report.approximated.size(), report.unsupported.size(), reportPath.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "nocturne-import: %s: %s\n", file.c_str(), e.what());
            failed++;
        }
    }
    return failed ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> positional;
    fs::path mapping, media, reportPath;
    std::vector<std::string> displays;
    azl::AvcOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&] {
            if (i + 1 >= argc)
                usage(2);
            return std::string(argv[++i]);
        };
        if (arg == "--mapping") mapping = value();
        else if (arg == "--media") media = value();
        else if (arg == "--displays") displays = split(value());
        else if (arg == "--start-column") options.startColumn = size_t(std::stoul(value()));
        else if (arg == "--report") reportPath = value();
        else if (arg == "--keep-clip-chains") options.hoistClipChains = false;
        else if (arg == "--help" || arg == "-h") usage(0);
        else if (arg.rfind("--", 0) == 0) usage(2);
        else positional.push_back(arg);
    }
    if (positional.size() >= 2 && isMapping(positional[0]))
        return importMappings({positional.begin(), positional.end() - 1}, positional.back(), displays);
    if (positional.size() != 2)
        usage(2);
    const fs::path source = positional[0];
    const fs::path out = fs::absolute(positional[1]);
    if (reportPath.empty())
        reportPath = out.parent_path() / (out.stem().string() + ".report.md");
    const fs::path mediaRoot = fs::absolute(media.empty() ? out.parent_path() : media);

    try {
        azl::ImportReport report;
        const auto avc = azl::parseXmlFile(source.string());
        azl::ImportedComposition imported =
            azl::importAvc(*avc, media.empty() ? azl::MediaFinder() : azl::mediaIn(mediaRoot), report, options);
        if (!mapping.empty())
            imported.composition.screens = azl::importAdvancedOutput(*azl::parseXmlFile(mapping.string()), displays, report);
        else
            report.mapped.push_back("no mapping given: the engine shows the composition on its first display");
        azl::writeFileAtomically(out, azl::portableFile(imported, mediaRoot).dump(2) + "\n");
        const std::string title = "Import of " + source.filename().string() +
                                  (mapping.empty() ? "" : " with " + mapping.filename().string());
        azl::writeFileAtomically(reportPath, azl::formatReport(report, title));
        std::printf("wrote %s\n  %zu imported, %zu approximated, %zu not imported; report: %s\n", out.c_str(),
                    report.mapped.size(), report.approximated.size(), report.unsupported.size(), reportPath.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nocturne-import: %s\n", e.what());
        return 1;
    }
    return 0;
}
