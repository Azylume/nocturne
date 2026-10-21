#include "effects/effect_registry.hpp"
#include "golden.hpp"
#include "test_client.hpp"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace {

using azl::AddEffect;

void addEffect(Client& client, const std::string& type, std::map<std::string, double> params = {})
{
    client.send(msg::command(AddEffect{1, std::nullopt, {0, type, std::move(params)}}));
    client.next();
}

// The parity captures' UV probe, R = x and G = y, the same effect the Classic parity tests register.
std::string uvProbe()
{
    const char* type = "parityUv";
    if (!azl::findEffect(type))
        azl::registerEffect({.type = type,
                             .kind = azl::StageKind::Pointwise,
                             .metal = "\n    float2 at = round(floor(px) * 255.0 / (h.size - 1.0));\n"
                                      "    return float4(at / 255.0, 128.0 / 255.0, 1.0);\n"});
    return type;
}

// A lone strip as the lit columns of the middle row, over the black a mode 0 Classic Stripper fills in around it.
struct Strip {
    int first = -1;
    int last = -1;
    bool operator==(const Strip&) const = default;
    bool shown() const { return first >= 0; }
};

Strip stripOf(const azl::Image& image)
{
    Strip strip;
    for (uint32_t x = 0; x < image.width; ++x) {
        const uint8_t* c = pixel(image, x, image.height / 2);
        if (c[0] + c[1] + c[2] <= 24)
            continue;
        strip.first = strip.first < 0 ? int(x) : strip.first;
        strip.last = int(x);
    }
    return strip;
}

const std::map<std::string, double> kOneStrip{{"number", 0.1}, {"globalWidth", 0.1}, {"mode", 0.0}, {"direction", 0.0},
                                              {"feather", 0.0}, {"speed", 0.0}};

std::map<std::string, double> with(std::map<std::string, double> params, const std::map<std::string, double>& more)
{
    for (const auto& [name, value] : more)
        params[name] = value;
    return params;
}

// A Classic Stripper over the UV probe, its strip in each of `frames` frames.
std::vector<Strip> strips(const std::map<std::string, double>& params, int frames, uint32_t width = 480,
                          uint32_t height = 270)
{
    azl::EngineHarness harness({.width = width, .height = height});
    Client client(harness.document());
    addEffect(client, uvProbe());
    addEffect(client, "classicStripper", params);
    std::vector<Strip> out;
    for (int i = 0; i < frames; ++i) {
        harness.step();
        out.push_back(stripOf(harness.readScreen()));
    }
    return out;
}

// How many frames in a row show the same place, the first and last run left out as cut short.
std::vector<int> runLengths(const std::vector<Strip>& frames)
{
    std::vector<int> runs{1};
    for (size_t i = 1; i < frames.size(); ++i) {
        if (frames[i] == frames[i - 1])
            ++runs.back();
        else
            runs.push_back(1);
    }
    if (runs.size() <= 2)
        return {};
    return {runs.begin() + 1, runs.end() - 1};
}

int median(std::vector<int> values)
{
    REQUIRE(!values.empty());
    std::nth_element(values.begin(), values.begin() + long(values.size() / 2), values.end());
    return values[values.size() / 2];
}

// The share of the places in a row of frames that leave the strip out of the picture, a place held over several
// frames counted once.
double outShare(const std::vector<Strip>& frames)
{
    int places = 0, out = 0;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (i > 0 && frames[i] == frames[i - 1])
            continue;
        ++places;
        out += !frames[i].shown();
    }
    return double(out) / places;
}

} // namespace

TEST_CASE("Classic Stripper MovementType picks Random from 0.8, so 0.75 .. 0.79 bounce as measured")
{
    const azl::ParamDef& movement = azl::findEffect("classicStripper")->params[4];
    REQUIRE(movement.name == "movementType");
    const std::vector<std::pair<double, int>> picks{{0.0, 0}, {0.24, 0}, {0.25, 1}, {0.5, 1}, {0.75, 1},
                                                    {0.773, 1}, {0.79, 1}, {0.8, 2}, {1.0, 2}};
    for (const auto& [value, option] : picks) {
        CAPTURE(value);
        CHECK(azl::optionIndex(movement, value) == option);
    }
    for (const double value : {0.75, 0.79}) {
        const std::vector<Strip> frames = strips(with(kOneStrip, {{"movementType", value}}), 20);
        CHECK(std::all_of(frames.begin(), frames.end(), [&](const Strip& s) { return s == frames[0]; }));
    }
    const std::vector<Strip> random = strips(with(kOneStrip, {{"movementType", 0.8}}), 20);
    CHECK(runLengths(random).size() >= 10);
}

TEST_CASE("Classic Stripper Random holds each place Speed x 10/3 s, anywhere over 1.5 picture widths")
{
    // 0.158 x 10/3 s is 31.6 frames.
    for (const double mode : {0.0, 0.5}) {
        CAPTURE(mode);
        const std::vector<int> runs =
            runLengths(strips(with(kOneStrip, {{"movementType", 1.0}, {"speed", 0.158}, {"mode", mode}}), 200));
        REQUIRE(runs.size() >= 4);
        CHECK(median(runs) >= 31);
        CHECK(median(runs) <= 32);
    }
    const std::vector<Strip> frames = strips(with(kOneStrip, {{"movementType", 1.0}}), 300);
    CHECK(median(runLengths(frames)) == 1);
    // A strip w units wide (the picture is 2) at a uniform place over 3 units is out of the picture 1 - (2 + w) / 3 of
    // the time, and never shows at both edges at once.
    int widest = 0;
    for (const Strip& s : frames)
        widest = std::max(widest, s.last - s.first + 1);
    CHECK(widest < 120);
    CHECK(outShare(frames) == doctest::Approx(1.0 - (2.0 + widest / 240.0) / 3.0).epsilon(0.3));
}

namespace {

// The reference moved Both Ways' strips by whole steps of 0.3 x Speed units, 7 to 9 of them between snapshots 65 to
// 83 ms apart: it rendered about 110 frames a second during the p11 capture (ticket 101), and moves strips per frame.
constexpr double kReferenceFps = 110.0;

azl::Image refImage(const fs::path& file)
{
    std::optional<azl::Image> image = loadPng(file.string());
    REQUIRE(image);
    return *image;
}

// Columns of the middle row neither black nor (within 8 levels) the input: the strip's soft edge.
int softColumns(const azl::Image& image, const azl::Image& input)
{
    int soft = 0;
    for (uint32_t x = 0; x < image.width; ++x) {
        const uint8_t* c = pixel(image, x, image.height / 2);
        const uint8_t* in = pixel(input, x, image.height / 2);
        const int level = c[0] + c[1] + c[2], full = in[0] + in[1] + in[2];
        soft += level > 8 && level < full - 8;
    }
    return soft;
}

} // namespace

// NOCTURNE_PARITY_DIR=DIR with the p11 `stripper-uv` work directory (tools/arena-capture): its MovementType and Feather
// stills and its MovementType 1.0 sequences against ours.
TEST_CASE("parity of Classic Stripper MovementType Random and Feather")
{
    const char* dir = std::getenv("NOCTURNE_PARITY_DIR");
    if (!dir || !fs::exists(fs::path(dir) / "seq/classicStripper.mt2.speed.0.0/frames.json")) {
        MESSAGE("skipped: set NOCTURNE_PARITY_DIR to the stripper-uv capture");
        return;
    }
    const fs::path root(dir);
    const nlohmann::json cases = nlohmann::json::parse(std::ifstream(root / "cases.json"))["cases"];
    azl::EngineHarness harness({.width = 1920, .height = 1080});
    Client client(harness.document());
    addEffect(client, uvProbe());
    harness.step();
    const azl::Image input = harness.readScreen();
    auto shown = [&](const azl::Image& image) {
        size_t same = 0;
        for (size_t i = 0; i < image.rgba.size(); i += 4)
            same += std::abs(int(image.rgba[i]) - input.rgba[i]) <= 8 && std::abs(int(image.rgba[i + 1]) - input.rgba[i + 1]) <= 8;
        return double(same) / double(image.rgba.size() / 4);
    };

    std::vector<Strip> refFrames, ourFrames;
    for (const auto& c : cases) {
        const std::string name = c["name"];
        if (!name.starts_with("classicStripper.mt") && !name.starts_with("classicStripper.feather"))
            continue;
        CAPTURE(name);
        std::map<std::string, double> params;
        for (const auto& [key, value] : c["params"].items())
            params[key] = value.get<double>();
        if (!c.value("sequence", false)) {
            const azl::Image ref = refImage(root / "ref" / (name + ".png"));
            azl::EngineHarness one({.width = 1920, .height = 1080});
            Client oneClient(one.document());
            addEffect(oneClient, uvProbe());
            addEffect(oneClient, "classicStripper", params);
            one.step(2);
            const azl::Image frame = one.readScreen();
            one.step(60);
            // A lit still settled, so it is at rest; a black one may be either.
            if (stripOf(ref).shown())
                CHECK(stripOf(one.readScreen()) == stripOf(frame));
            CHECK(std::abs(shown(frame) - shown(ref)) <= 0.1);
            CHECK(softColumns(ref, input) <= 2);
            CHECK(softColumns(frame, input) <= 2);
            continue;
        }
        const fs::path seq = root / "seq" / (name + ".0");
        const nlohmann::json frames = nlohmann::json::parse(std::ifstream(seq / "frames.json"))["frames"];
        std::vector<Strip> ref;
        std::vector<double> times;
        for (const auto& f : frames) {
            ref.push_back(stripOf(refImage(seq / f["file"].get<std::string>())));
            times.push_back((f["asked"].get<double>() + f["came"].get<double>()) / 2.0);
        }
        // The reference's hold: the median time between its jumps, or under a snapshot when it jumps every snapshot.
        std::vector<double> jumps, gaps;
        for (size_t i = 1; i < ref.size(); ++i) {
            gaps.push_back(times[i] - times[i - 1]);
            if (!(ref[i] == ref[i - 1]))
                jumps.push_back((times[i] + times[i - 1]) / 2.0);
        }
        std::sort(gaps.begin(), gaps.end());
        const double snapshot = gaps[gaps.size() / 2];
        std::vector<double> holds;
        for (size_t i = 1; i < jumps.size(); ++i)
            holds.push_back(jumps[i] - jumps[i - 1]);
        std::sort(holds.begin(), holds.end());
        const double refHold = holds[holds.size() / 2];

        const std::vector<Strip> ours = strips(params, 300);
        const double ourHold = median(runLengths(ours)) / kReferenceFps;
        MESSAGE(name << ": reference hold " << refHold << " s (snapshots " << snapshot << " s apart), ours "
                     << ourHold << " s at the reference's frame rate");
        if (refHold < 1.5 * snapshot)
            CHECK(ourHold < snapshot);
        else
            CHECK(ourHold == doctest::Approx(refHold).epsilon(0.15));
        refFrames.insert(refFrames.end(), ref.begin(), ref.end());
        ourFrames.insert(ourFrames.end(), ours.begin(), ours.end());
    }
    REQUIRE(!refFrames.empty());
    MESSAGE("out of the picture: reference " << outShare(refFrames) << ", ours " << outShare(ourFrames));
    CHECK(std::abs(outShare(refFrames) - outShare(ourFrames)) <= 0.1);
}
