#pragma once

#include "HostPlatform.h"

#include <memory>
#include <string>

namespace simulator {

class RepeaterFirmware {
  class Impl;
  std::unique_ptr<Impl> impl_;

public:
  RepeaterFirmware(const DatabaseNode& node, size_t index, SimulatedMedium& medium,
                   SimClock& clock, const LoRaConfig& config, uint32_t seed);
  ~RepeaterFirmware();
  RepeaterFirmware(const RepeaterFirmware&) = delete;
  RepeaterFirmware& operator=(const RepeaterFirmware&) = delete;

  void begin();
  void loop();
  void injectAdvert();
  const std::string& id() const;
  uint64_t sentFlood() const;
  uint64_t sentDirect() const;
  uint64_t receivedFlood() const;
  uint64_t receivedDirect() const;
  bool identityMatchesDatabasePrefix() const;
  const LoRaConfig& radioConfig() const;
};

}  // namespace simulator
