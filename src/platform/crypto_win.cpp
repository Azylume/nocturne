#include "platform/crypto.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace azl {

namespace {

void check(NTSTATUS status, const char* what)
{
    if (!BCRYPT_SUCCESS(status))
        throw std::runtime_error(std::string("BCrypt ") + what + " failed");
}

PUCHAR bytes(const void* data)
{
    return static_cast<PUCHAR>(const_cast<void*>(data));
}

} // namespace

struct Sha256::Impl {
    BCRYPT_HASH_HANDLE hash = nullptr;
};

Sha256::Sha256() : impl_(std::make_unique<Impl>())
{
    check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &impl_->hash, nullptr, 0, nullptr, 0, 0), "create hash");
}

Sha256::~Sha256()
{
    if (impl_->hash)
        BCryptDestroyHash(impl_->hash);
}

void Sha256::update(const void* data, size_t size)
{
    check(BCryptHashData(impl_->hash, bytes(data), ULONG(size), 0), "hash data");
}

Sha256Digest Sha256::finish()
{
    Sha256Digest digest;
    const NTSTATUS status = BCryptFinishHash(impl_->hash, digest.data(), ULONG(digest.size()), 0);
    BCryptDestroyHash(std::exchange(impl_->hash, nullptr));
    check(status, "finish hash");
    return digest;
}

Sha256Digest sha256(const void* data, size_t size)
{
    Sha256Digest digest;
    check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes(data), ULONG(size), digest.data(), ULONG(digest.size())),
          "hash");
    return digest;
}

Sha256Digest hmacSha256(const void* key, size_t keySize, const void* data, size_t size)
{
    Sha256Digest digest;
    check(BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, bytes(key), ULONG(keySize), bytes(data), ULONG(size), digest.data(),
                     ULONG(digest.size())),
          "HMAC");
    return digest;
}

void randomBytes(void* out, size_t size)
{
    check(BCryptGenRandom(nullptr, bytes(out), ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG), "random");
}

} // namespace azl
