#include <doctest.h>

#include "platform/crypto.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace {

std::string hex(const azl::Sha256Digest& digest)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (uint8_t b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

} // namespace

TEST_CASE("platform: SHA-256 and HMAC-SHA-256 match their published test vectors")
{
    CHECK(hex(azl::sha256("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex(azl::sha256("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    azl::Sha256 parts;
    parts.update("a", 1);
    parts.update("bc", 2);
    CHECK(hex(parts.finish()) == hex(azl::sha256("abc", 3)));
    const std::string key = "Jefe", data = "what do ya want for nothing?";
    CHECK(hex(azl::hmacSha256(key.data(), key.size(), data.data(), data.size())) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("platform: random bytes differ from draw to draw")
{
    std::array<uint8_t, 32> a{}, b{};
    azl::randomBytes(a.data(), a.size());
    azl::randomBytes(b.data(), b.size());
    CHECK(a != b);
    CHECK(a != std::array<uint8_t, 32>{});
}
