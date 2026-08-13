#pragma once

#include <cstddef>
#include <cstdint>

inline int decode_base64(const unsigned char* source, size_t length, unsigned char* destination) {
  auto value = [](unsigned char character) -> int {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+') return 62;
    if (character == '/') return 63;
    return -1;
  };
  uint32_t accumulator = 0;
  int bits = 0;
  int written = 0;
  for (size_t index = 0; index < length; ++index) {
    if (source[index] == '=') break;
    const int decoded = value(source[index]);
    if (decoded < 0) continue;
    accumulator = (accumulator << 6) | static_cast<uint32_t>(decoded);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      destination[written++] = static_cast<unsigned char>(accumulator >> bits);
      accumulator &= (1U << bits) - 1U;
    }
  }
  return written;
}

