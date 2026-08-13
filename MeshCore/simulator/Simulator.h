#pragma once

#include <Dispatcher.h>
#include <Mesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace simulator {

struct LoRaConfig {
  double frequency_mhz = 915.0;
  double bandwidth_khz = 250.0;
  uint8_t spreading_factor = 10;
  uint8_t coding_rate_denominator = 5;
  uint16_t preamble_symbols = 16;
  bool explicit_header = true;
  bool crc = true;

  uint32_t airtimeMillis(size_t payload_bytes) const;
  void validate() const;
};

struct MediumStats {
  uint64_t transmissions = 0;
  uint64_t delivered = 0;
  uint64_t collided_transmissions = 0;
  uint64_t collision_drops = 0;
  uint64_t incompatible_drops = 0;
  uint64_t interference_drops = 0;
};

class SimClock : public mesh::MillisecondClock, public mesh::RTCClock {
  uint64_t now_ms_ = 0;
  uint32_t epoch_ = 0;

public:
  explicit SimClock(uint32_t epoch = 1'700'000'000) : epoch_(epoch) {}
  unsigned long getMillis() override { return static_cast<unsigned long>(now_ms_); }
  uint32_t getCurrentTime() override { return epoch_ + static_cast<uint32_t>(now_ms_ / 1000); }
  void setCurrentTime(uint32_t value) override { epoch_ = value - static_cast<uint32_t>(now_ms_ / 1000); }
  uint64_t now() const { return now_ms_; }
  void set(uint64_t value) { now_ms_ = value; }
};

class SimRng : public mesh::RNG {
  std::mt19937 engine_;

public:
  explicit SimRng(uint32_t seed) : engine_(seed) {}
  void random(uint8_t* destination, size_t size) override;
};

class SimulatedRadio;

class SimulatedMedium {
public:
  struct Node {
    std::string id;
    std::unordered_set<size_t> outgoing;
  };

private:
  struct Transmission {
    size_t sender;
    uint64_t started_at;
    uint64_t ends_at;
    std::vector<uint8_t> bytes;
    LoRaConfig config;
    std::vector<size_t> observable_receivers;
    bool finalised = false;
  };

  SimClock& clock_;
  double interference_;
  std::mt19937 random_;
  std::vector<Node> nodes_;
  std::unordered_map<std::string, size_t> indexes_;
  std::vector<SimulatedRadio*> radios_;
  std::vector<Transmission> transmissions_;
  MediumStats stats_;
  std::function<void(const std::string&, const std::string&, uint64_t,
                     const std::vector<uint8_t>&)> delivery_observer_;
  std::function<void(const std::string&, uint64_t,
                     const std::vector<uint8_t>&)> send_observer_;
  std::function<void(const std::string&, const std::string&, bool, uint64_t,
                     uint32_t, const std::vector<uint8_t>&)> transmission_observer_;

  bool connected(size_t sender, size_t receiver) const;
  bool overlapping(const Transmission& left, const Transmission& right) const;
  bool sameChannel(const LoRaConfig& left, const LoRaConfig& right) const;
  bool receiverCompatible(const LoRaConfig& transmission, const LoRaConfig& receiver) const;
  void finalise(Transmission& transmission);

public:
  SimulatedMedium(SimClock& clock, double interference, uint32_t seed);
  size_t addNode(const std::string& id);
  void addLink(const std::string& source, const std::string& target, bool undirected);
  void attach(size_t node, SimulatedRadio* radio);
  uint64_t transmit(size_t sender, const uint8_t* bytes, size_t length,
                    uint32_t airtime_ms, const LoRaConfig& config);
  bool channelBusy(size_t receiver) const;
  void advance();
  void setDeliveryObserver(
      std::function<void(const std::string&, const std::string&, uint64_t,
                         const std::vector<uint8_t>&)> observer) {
    delivery_observer_ = std::move(observer);
  }
  void setSendObserver(
      std::function<void(const std::string&, uint64_t,
                         const std::vector<uint8_t>&)> observer) {
    send_observer_ = std::move(observer);
  }
  void setTransmissionObserver(
      std::function<void(const std::string&, const std::string&, bool, uint64_t,
                         uint32_t, const std::vector<uint8_t>&)> observer) {
    transmission_observer_ = std::move(observer);
  }
  size_t nodeCount() const { return nodes_.size(); }
  const std::vector<Node>& nodes() const { return nodes_; }
  const MediumStats& stats() const { return stats_; }
};

class SimulatedRadio : public mesh::Radio {
  struct Datagram {
    std::vector<uint8_t> bytes;
    float snr;
    float rssi;
  };

  SimulatedMedium& medium_;
  SimClock& clock_;
  LoRaConfig config_;
  size_t node_;
  uint64_t transmitting_until_ = 0;
  std::deque<Datagram> inbox_;
  float last_snr_ = 10.0f;
  float last_rssi_ = -80.0f;
  uint32_t packets_received_ = 0;
  uint32_t packets_sent_ = 0;

public:
  SimulatedRadio(SimulatedMedium& medium, SimClock& clock, LoRaConfig config, size_t node);
  int recvRaw(uint8_t* bytes, int size) override;
  uint32_t getEstAirtimeFor(int length) override;
  float packetScore(float snr, int packet_length) override;
  bool startSendRaw(const uint8_t* bytes, int length) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isReceiving() override;
  float getLastRSSI() const override { return last_rssi_; }
  float getLastSNR() const override { return last_snr_; }
  void deliver(const std::vector<uint8_t>& bytes, float snr = 10.0f, float rssi = -80.0f);
  void configure(const LoRaConfig& config);
  void setParams(float frequency, float bandwidth, uint8_t spreading_factor,
                 uint8_t coding_rate);
  const LoRaConfig& config() const { return config_; }
  uint32_t packetsReceived() const { return packets_received_; }
  uint32_t packetsSent() const { return packets_sent_; }
  void resetStats() { packets_received_ = packets_sent_ = 0; }
};

struct DatabaseNode {
  std::string id;
  std::string public_key;
};

struct DatabaseLink {
  std::string source;
  std::string target;
};

struct DatabaseTopology {
  std::vector<DatabaseNode> nodes;
  std::vector<DatabaseLink> links;
};

DatabaseTopology loadTopology(const std::string& path, size_t node_limit = 0);

}  // namespace simulator
