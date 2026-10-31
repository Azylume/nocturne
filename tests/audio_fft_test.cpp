#include "audio/audio_analyzer.hpp"
#include "techno_loop.hpp"

#include <doctest.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using azl::AudioFrame;

// The analysis reference: every 16th frame of two bars of the techno loop, analysed on a Mac through vDSP.
const std::string kReference = std::string(NOCTURNE_GOLDEN_DIR) + "/audio/techno_analysis.txt";
constexpr size_t kReferenceEvery = 16;
// Band envelopes may differ by this much (of 0..1) across platforms: libm and fused multiply-adds round differently.
constexpr float kValueTolerance = 1e-3f;
// The spectrum (0..1 over 60 dB) may differ by this much: 0.06 dB.
constexpr float kSpectrumTolerance = 1e-3f;

std::vector<AudioFrame> analyse(std::unique_ptr<azl::PowerSpectrum> transform)
{
    const std::vector<float> samples = TechnoLoop{.bars = 2}.render();
    azl::AudioAnalyzer analyzer(TechnoLoop::kRate, {}, azl::AudioAnalyzer::kDefaultEdges, std::move(transform));
    std::vector<AudioFrame> frames;
    analyzer.process(samples.data(), samples.size(), 0.0, true, [&](const AudioFrame& frame) { frames.push_back(frame); });
    return frames;
}

template <size_t N>
float worst(const std::array<float, N>& a, const std::array<float, N>& b)
{
    float result = 0.0f;
    for (size_t i = 0; i < N; ++i)
        result = std::max(result, std::abs(a[i] - b[i]));
    return result;
}

#ifdef __APPLE__
void write(const std::vector<AudioFrame>& frames)
{
    std::ofstream out(kReference);
    out.precision(9);
    for (size_t i = 0; i < frames.size(); i += kReferenceEvery) {
        const AudioFrame& f = frames[i];
        out << i << ' ' << f.time;
        for (uint64_t onsets : f.onsets)
            out << ' ' << onsets;
        for (float v : f.values)
            out << ' ' << v;
        for (float v : f.spectrum)
            out << ' ' << v;
        out << '\n';
    }
}
#endif

struct Reference {
    size_t index = 0;
    AudioFrame frame;
};

std::vector<Reference> read()
{
    std::ifstream in(kReference);
    std::vector<Reference> result;
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        Reference r;
        fields >> r.index >> r.frame.time;
        for (uint64_t& onsets : r.frame.onsets)
            fields >> onsets;
        for (float& v : r.frame.values)
            fields >> v;
        for (float& v : r.frame.spectrum)
            fields >> v;
        REQUIRE(fields);
        result.push_back(r);
    }
    return result;
}

} // namespace

#ifdef __APPLE__
TEST_CASE("the spectrum through pocketfft matches vDSP; band envelopes and onsets do not depend on the transform")
{
    const std::vector<AudioFrame> vdsp = analyse(azl::makeVdspSpectrum(azl::AudioAnalyzer::kFftLog2));
    const std::vector<AudioFrame> pocket = analyse(azl::makePocketSpectrum(azl::AudioAnalyzer::kFftLog2));
    REQUIRE(vdsp.size() == pocket.size());
    float spectrum = 0.0f;
    for (size_t i = 0; i < vdsp.size(); ++i) {
        CHECK(vdsp[i].values == pocket[i].values);
        CHECK(vdsp[i].onsets == pocket[i].onsets);
        spectrum = std::max(spectrum, worst(vdsp[i].spectrum, pocket[i].spectrum));
    }
    MESSAGE("largest spectrum difference: " << spectrum);
    CHECK(spectrum <= 1e-4f);
}
#endif

TEST_CASE("the techno loop's band envelopes, onsets and spectrum match the reference analysed on a Mac")
{
    const std::vector<AudioFrame> frames = analyse(nullptr);
#ifdef __APPLE__
    const char* update = std::getenv("NOCTURNE_UPDATE_GOLDEN");
    if (update && std::string(update) == "1")
        write(frames);
#endif
    const std::vector<Reference> reference = read();
    REQUIRE(reference.size() == (frames.size() + kReferenceEvery - 1) / kReferenceEvery);
    float values = 0.0f;
    float spectrum = 0.0f;
    for (const Reference& r : reference) {
        const AudioFrame& f = frames[r.index];
        CAPTURE(r.index);
        CHECK(f.time == doctest::Approx(r.frame.time).epsilon(1e-7));
        CHECK(f.onsets == r.frame.onsets);
        values = std::max(values, worst(f.values, r.frame.values));
        spectrum = std::max(spectrum, worst(f.spectrum, r.frame.spectrum));
    }
    MESSAGE("largest difference: envelopes " << values << ", spectrum " << spectrum);
    CHECK(values <= kValueTolerance);
    CHECK(spectrum <= kSpectrumTolerance);
    CHECK(reference.back().frame.onsets[0] > 0);
}
