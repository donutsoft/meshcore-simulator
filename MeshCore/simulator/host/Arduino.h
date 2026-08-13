#pragma once

#include "Stream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

using String = std::string;

template <typename T, typename U> constexpr auto min(T left, U right) {
  using R = decltype(left + right);
  return static_cast<R>(left) < static_cast<R>(right) ? static_cast<R>(left) : static_cast<R>(right);
}
template <typename T, typename U> constexpr auto max(T left, U right) {
  using R = decltype(left + right);
  return static_cast<R>(left) > static_cast<R>(right) ? static_cast<R>(left) : static_cast<R>(right);
}
template <typename T, typename L, typename H> constexpr T constrain(T value, L low, H high) {
  if (value < static_cast<T>(low)) return static_cast<T>(low);
  if (value > static_cast<T>(high)) return static_cast<T>(high);
  return value;
}

unsigned long millis();
inline void delay(unsigned long) {}
void randomSeed(unsigned long seed);
long random(long maximum);
long random(long minimum, long maximum);
char* ltoa(long value, char* destination, int base);

class HostSerial final : public Stream {
public:
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t*, size_t size) override { return size; }
};

extern HostSerial Serial;
