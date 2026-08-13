#pragma once

#include "Simulator.h"

#include <FS.h>
#include <MeshCore.h>
#include <helpers/SensorManager.h>

#include <cstdint>

namespace simulator {

struct FirmwareContext {
  SimulatedRadio* radio = nullptr;
  SimClock* clock = nullptr;
  mesh::RNG* rng = nullptr;
  fs::FS* filesystem = nullptr;
  bool reboot_requested = false;
};

FirmwareContext* activeFirmwareContext();

class FirmwareContextGuard {
  FirmwareContext* previous_;

public:
  explicit FirmwareContextGuard(FirmwareContext& context);
  ~FirmwareContextGuard();
};

}  // namespace simulator

class HostBoard final : public mesh::MainBoard {
public:
  uint16_t getBattMilliVolts() override { return 4200; }
  const char* getManufacturerName() const override { return "MeshCore Simulator"; }
  void reboot() override;
  uint8_t getStartupReason() const override { return BD_STARTUP_NORMAL; }
};

class HostRTCClockProxy final : public mesh::RTCClock {
public:
  uint32_t getCurrentTime() override;
  void setCurrentTime(uint32_t time) override;
};

class HostRadioDriver {
public:
  void setParams(float frequency, float bandwidth, uint8_t spreading_factor, uint8_t coding_rate);
  void setTxPower(int8_t) {}
  bool setRxBoostedGainMode(bool enabled) { return enabled; }
  bool getRxBoostedGainMode() const { return false; }
  float getLastRSSI() const;
  float getLastSNR() const;
  uint32_t getPacketsRecv() const;
  uint32_t getPacketsSent() const;
  uint32_t getPacketsRecvErrors() const { return 0; }
  uint32_t getRngSeed() const { return 0x4d657368; }
  void resetStats();
};
