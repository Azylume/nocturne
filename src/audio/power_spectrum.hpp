#pragma once

#include <cstddef>
#include <memory>

namespace azl {

// The power |X_k|^2 of bins 0 to n/2 - 1 of the unscaled real DFT of n = 2^log2 samples under a periodic Hann window
// with a mean square of 1: sqrt(8/3) (0.5 - 0.5 cos(2 pi i / n)), vDSP's normalised Hann. Owns its buffers; one caller
// at a time.
class PowerSpectrum {
public:
    virtual ~PowerSpectrum() = default;
    // `samples` holds n values, `power` n / 2.
    virtual void compute(const float* samples, float* power) = 0;
};

// The platform's transform: vDSP on Apple, pocketfft elsewhere.
std::unique_ptr<PowerSpectrum> makePowerSpectrum(size_t log2);
std::unique_ptr<PowerSpectrum> makePocketSpectrum(size_t log2);
#ifdef __APPLE__
std::unique_ptr<PowerSpectrum> makeVdspSpectrum(size_t log2);
#endif

} // namespace azl
