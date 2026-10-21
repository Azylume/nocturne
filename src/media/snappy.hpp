#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace azl {

// The raw Snappy format (google/snappy format_description.txt), without the framing format.

// The uncompressed length a compressed block declares; none when the preamble is malformed.
std::optional<size_t> snappyLength(const uint8_t* data, size_t size);
// Decompresses into exactly `outSize` bytes. False when the data is malformed or does not fill `outSize` exactly.
bool snappyDecompress(const uint8_t* data, size_t size, uint8_t* out, size_t outSize);
std::vector<uint8_t> snappyCompress(const uint8_t* data, size_t size);

} // namespace azl
