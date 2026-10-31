#include "platform/crypto.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>

#include <cstdlib>

namespace azl {

struct Sha256::Impl {
    CC_SHA256_CTX context;
};

Sha256::Sha256() : impl_(std::make_unique<Impl>())
{
    CC_SHA256_Init(&impl_->context);
}

Sha256::~Sha256() = default;

void Sha256::update(const void* data, size_t size)
{
    CC_SHA256_Update(&impl_->context, data, CC_LONG(size));
}

Sha256Digest Sha256::finish()
{
    Sha256Digest digest;
    CC_SHA256_Final(digest.data(), &impl_->context);
    return digest;
}

Sha256Digest sha256(const void* data, size_t size)
{
    Sha256Digest digest;
    CC_SHA256(data, CC_LONG(size), digest.data());
    return digest;
}

Sha256Digest hmacSha256(const void* key, size_t keySize, const void* data, size_t size)
{
    Sha256Digest digest;
    CCHmac(kCCHmacAlgSHA256, key, keySize, data, size, digest.data());
    return digest;
}

void randomBytes(void* out, size_t size)
{
    arc4random_buf(out, size);
}

} // namespace azl
