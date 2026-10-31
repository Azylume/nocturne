#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace azl {

using Sha256Digest = std::array<uint8_t, 32>;

// SHA-256 over data fed in parts (CommonCrypto on Apple, BCrypt on Windows). One thread at a time.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const void* data, size_t size);
    // Ends the hash; call once.
    Sha256Digest finish();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Any thread.
Sha256Digest sha256(const void* data, size_t size);
Sha256Digest hmacSha256(const void* key, size_t keySize, const void* data, size_t size);
// Bytes from the system's cryptographic random source. Any thread.
void randomBytes(void* out, size_t size);

} // namespace azl
