#pragma once

#include "engine/screen_painter.hpp"
#include "output/recorder.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace azl {

// The screen a recording's target names, none for the whole composition; throws std::runtime_error for an unknown
// target.
std::optional<uint64_t> screenOf(const std::string& target, const Composition& composition);
// What a recording of `screen` (none: the whole composition) paints, through the screen's mapping.
ScreenView recordedView(std::optional<uint64_t> screen, const Composition& composition);

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
