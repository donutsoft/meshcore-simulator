#pragma once

#include <helpers/BaseSerialInterface.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace simulator {

class TcpSerialInterface final : public BaseSerialInterface {
  std::string bind_address_;
  uint16_t port_;
  mutable std::mutex mutex_;
  std::deque<std::vector<uint8_t>> input_queue_;
  std::deque<std::vector<uint8_t>> output_queue_;
  std::thread thread_;
  std::atomic<bool> enabled_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> stopping_{false};
  int listener_ = -1;
  int client_ = -1;

  void runServer();
  void closeSockets();

public:
  TcpSerialInterface(std::string bind_address, uint16_t port);
  ~TcpSerialInterface();
  void start();
  void stop();

  void enable() override { enabled_.store(true); }
  void disable() override;
  bool isEnabled() const override { return enabled_.load(); }
  bool isConnected() const override { return connected_.load(); }
  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t source[], size_t length) override;
  size_t checkRecvFrame(uint8_t destination[]) override;
  uint16_t port() const { return port_; }
};

}  // namespace simulator
