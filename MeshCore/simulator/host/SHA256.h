#pragma once

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

class SHA256 {
  CC_SHA256_CTX digest_{};
  CCHmacContext hmac_{};
  bool hmac_mode_ = false;

public:
  SHA256() { CC_SHA256_Init(&digest_); }
  void update(const void* data, size_t length) {
    if (hmac_mode_)
      CCHmacUpdate(&hmac_, data, length);
    else
      CC_SHA256_Update(&digest_, data, static_cast<CC_LONG>(length));
  }
  void finalize(void* output, size_t length) {
    uint8_t full[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(full, &digest_);
    std::memcpy(output, full, std::min(length, sizeof(full)));
  }
  void resetHMAC(const uint8_t* key, size_t length) {
    hmac_mode_ = true;
    CCHmacInit(&hmac_, kCCHmacAlgSHA256, key, length);
  }
  void finalizeHMAC(const uint8_t*, size_t, void* output, size_t output_length) {
    uint8_t full[CC_SHA256_DIGEST_LENGTH];
    CCHmacFinal(&hmac_, full);
    std::memcpy(output, full, std::min(output_length, sizeof(full)));
  }
};
