#pragma once

#include "harness/engine_harness.hpp"

#include <optional>
#include <string>

// Checks `actual` against tests/golden/<name>.png within a small per-pixel tolerance. With NOCTURNE_UPDATE_GOLDEN=1 in
// the environment it rewrites the golden instead. On a mismatch the actual image is written next to the test binary.
void checkGolden(const azl::Image& actual, const std::string& name);
void savePng(azl::Image image, const std::string& path);
std::optional<azl::Image> loadPng(const std::string& path);
