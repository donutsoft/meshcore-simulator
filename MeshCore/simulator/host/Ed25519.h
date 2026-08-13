#pragma once

#include <ed_25519.h>
#include <cstddef>
#include <cstdint>

class Ed25519 {
public:
  static bool verify(const uint8_t* signature, const uint8_t* public_key,
                     const uint8_t* message, size_t message_length) {
    return ed25519_verify(signature, message, message_length, public_key) == 1;
  }
};

