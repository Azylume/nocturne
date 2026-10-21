#pragma once

namespace azl::restart {

// The exit codes with which the engine asks nocturne-supervisor to start it again: in normal mode (from safe mode, this
// leaves it), or in safe mode.
constexpr int kNormal = 75;
constexpr int kSafeMode = 76;

// Set in the engine's environment by the supervisor, so the engine knows someone hears the request.
constexpr const char* kSupervisedVariable = "NOCTURNE_SUPERVISED";

} // namespace azl::restart
