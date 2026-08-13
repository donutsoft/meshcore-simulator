#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class CayenneLPP {
  std::vector<uint8_t> buffer_;
  size_t maximum_;

public:
  explicit CayenneLPP(size_t maximum) : maximum_(maximum) { buffer_.reserve(maximum); }
  void reset() { buffer_.clear(); }
  uint8_t* getBuffer() { return buffer_.data(); }
  const uint8_t* getBuffer() const { return buffer_.data(); }
  uint16_t getSize() const { return static_cast<uint16_t>(buffer_.size()); }
  bool addVoltage(uint8_t channel, float voltage);
  bool addTemperature(uint8_t channel, float temperature);
};

