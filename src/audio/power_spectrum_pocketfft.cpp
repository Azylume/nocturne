#include "audio/power_spectrum.hpp"

#define POCKETFFT_NO_MULTITHREADING
#include <pocketfft_hdronly.h>

#include <cmath>
#include <numbers>
#include <vector>

namespace azl {

namespace {

class PocketSpectrum final : public PowerSpectrum {
public:
    explicit PocketSpectrum(size_t log2) : n_(size_t(1) << log2), plan_(n_), window_(n_), data_(n_)
    {
        const double scale = std::sqrt(8.0 / 3.0);
        for (size_t i = 0; i < n_; ++i)
            window_[i] = float(scale * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(n_))));
    }

    // pocketfft's real transform leaves r0, r1, i1, r2, i2, ... in place.
    void compute(const float* samples, float* power) override
    {
        for (size_t i = 0; i < n_; ++i)
            data_[i] = samples[i] * window_[i];
        plan_.exec(data_.data(), 1.0f, true);
        power[0] = data_[0] * data_[0];
        for (size_t k = 1; k < n_ / 2; ++k)
            power[k] = data_[2 * k - 1] * data_[2 * k - 1] + data_[2 * k] * data_[2 * k];
    }

private:
    size_t n_;
    pocketfft::detail::pocketfft_r<float> plan_;
    std::vector<float> window_;
    std::vector<float> data_;
};

} // namespace

std::unique_ptr<PowerSpectrum> makePocketSpectrum(size_t log2)
{
    return std::make_unique<PocketSpectrum>(log2);
}

#ifndef __APPLE__
std::unique_ptr<PowerSpectrum> makePowerSpectrum(size_t log2)
{
    return makePocketSpectrum(log2);
}
#endif

} // namespace azl
