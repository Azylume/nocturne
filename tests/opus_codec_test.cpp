#include "audio/opus_codec.hpp"
#include "audio/return_audio.hpp"
#include "tone.hpp"

#include <doctest.h>

#include <algorithm>

TEST_CASE("return audio: the Opus codec carries a tone through at 44.1 and 48 kHz, chunked as an input")
{
    for (const double rate : {44100.0, 48000.0}) {
        INFO(rate << " Hz in");
        azl::OpusStreamEncoder encoder(rate, azl::ReturnAudio::kBitrate);
        azl::OpusStreamDecoder decoder;
        const std::vector<float> in = tone(rate, 2.0);
        std::vector<float> out;
        size_t packets = 0, bytes = 0;
        for (size_t at = 0; at < in.size(); at += 441)
            for (const auto& packet : encoder.encode(in.data() + at, std::min<size_t>(441, in.size() - at))) {
                ++packets;
                bytes += packet.size();
                CHECK(decoder.decode(packet.data(), packet.size(), out));
            }
        CHECK(packets >= 95);
        CHECK(packets <= 100);
        CHECK(double(bytes) * 8.0 / 2.0 <= azl::ReturnAudio::kBitrate * 1.15);
        const Heard heard = hear(out);
        CHECK(heard.hz == doctest::Approx(kTone).epsilon(0.01));
        CHECK(heard.rms == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.1));
    }
}
