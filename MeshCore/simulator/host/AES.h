#pragma once

#include <CommonCrypto/CommonCryptor.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

class AES128 {
  uint8_t key_[kCCKeySizeAES128]{};

  void crypt(CCOperation operation, uint8_t* output, const uint8_t* input) {
    size_t written = 0;
    CCCrypt(operation, kCCAlgorithmAES, kCCOptionECBMode, key_, sizeof(key_), nullptr,
            input, kCCBlockSizeAES128, output, kCCBlockSizeAES128, &written);
  }

public:
  void setKey(const uint8_t* key, size_t size) {
    std::memset(key_, 0, sizeof(key_));
    std::memcpy(key_, key, size < sizeof(key_) ? size : sizeof(key_));
  }
  void encryptBlock(uint8_t* output, const uint8_t* input) { crypt(kCCEncrypt, output, input); }
  void decryptBlock(uint8_t* output, const uint8_t* input) { crypt(kCCDecrypt, output, input); }
};

