#pragma once

#include "Simulator.h"

#include <cstdint>
#include <memory>
#include <string>

namespace simulator {

class CompanionEndpoint {
  class Impl;
  std::unique_ptr<Impl> impl_;

public:
  CompanionEndpoint(const std::string& repeater_id, const std::string& bind_address,
                    uint16_t port, SimulatedMedium& medium, SimClock& clock,
                    const LoRaConfig& config, size_t node_index,
                    const std::string& storage_image);
  ~CompanionEndpoint();
  CompanionEndpoint(const CompanionEndpoint&) = delete;
  CompanionEndpoint& operator=(const CompanionEndpoint&) = delete;

  void start();
  void stop();
  void pump();
  const std::string& repeaterId() const;
  uint16_t port() const;
  bool connected() const;
};

}  // namespace simulator
