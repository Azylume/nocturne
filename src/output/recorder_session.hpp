#pragma once

#include "output/recorder.hpp"

#include <memory>
#include <string>

namespace azl {

// One recording: the writer, and the platform's way of filling its pictures from the engine's frames on each frame's
// command buffer (Recorder::PlatformSession in recorder.mm, recorder_ffmpeg.cpp).
struct Recorder::Session : FrameTap, std::enable_shared_from_this<Session> {
    // The pixel format handed to the encoder, for the status.
    virtual std::string pixels() const = 0;
    // Until the encoder lets go of the pictures it was handed, or about a second.
    virtual void waitForEncoder() = 0;

    std::unique_ptr<recording::Writer> writer;
    AudioAnalysis* analysis = nullptr;
};

} // namespace azl
