#include "audio/power_spectrum.hpp"

#include <Accelerate/Accelerate.h>

#include <vector>

namespace azl {

namespace {

class VdspSpectrum final : public PowerSpectrum {
public:
    explicit VdspSpectrum(size_t log2)
        : log2_(log2), n_(size_t(1) << log2), setup_(vDSP_create_fftsetup(log2, kFFTRadix2)), window_(n_),
          windowed_(n_), real_(n_ / 2), imag_(n_ / 2)
    {
        vDSP_hann_window(window_.data(), n_, vDSP_HANN_NORM);
    }
    ~VdspSpectrum() override { vDSP_destroy_fftsetup(setup_); }

    void compute(const float* samples, float* power) override
    {
        for (size_t i = 0; i < n_; ++i)
            windowed_[i] = samples[i] * window_[i];
        DSPSplitComplex split{real_.data(), imag_.data()};
        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(windowed_.data()), 2, &split, 1, n_ / 2);
        vDSP_fft_zrip(setup_, &split, 1, log2_, kFFTDirection_Forward);
        split.imagp[0] = 0.0f;
        vDSP_zvmags(&split, 1, power, 1, n_ / 2);
        // vDSP's real transform is twice the DFT.
        for (size_t k = 0; k < n_ / 2; ++k)
            power[k] *= 0.25f;
    }

private:
    size_t log2_;
    size_t n_;
    FFTSetup setup_;
    std::vector<float> window_;
    std::vector<float> windowed_;
    std::vector<float> real_;
    std::vector<float> imag_;
};

} // namespace

std::unique_ptr<PowerSpectrum> makeVdspSpectrum(size_t log2)
{
    return std::make_unique<VdspSpectrum>(log2);
}

std::unique_ptr<PowerSpectrum> makePowerSpectrum(size_t log2)
{
    return makeVdspSpectrum(log2);
}

} // namespace azl
