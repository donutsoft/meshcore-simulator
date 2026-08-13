#include "Simulator.h"

#include <MeshCore.h>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace simulator {

uint32_t LoRaConfig::airtimeMillis(size_t payload_bytes) const {
  validate();
  const double bandwidth_hz = bandwidth_khz * 1000.0;
  const double symbol_seconds = std::pow(2.0, spreading_factor) / bandwidth_hz;
  const int low_data_rate_optimisation = symbol_seconds >= 0.016 ? 1 : 0;
  const int implicit_header = explicit_header ? 0 : 1;
  const int crc_enabled = crc ? 1 : 0;
  const double numerator = 8.0 * payload_bytes - 4.0 * spreading_factor + 28.0 +
                           16.0 * crc_enabled - 20.0 * implicit_header;
  const double denominator = 4.0 * (spreading_factor - 2 * low_data_rate_optimisation);
  const double payload_groups = std::max(0.0, std::ceil(numerator / denominator));
  const double payload_symbols = 8.0 + payload_groups * coding_rate_denominator;
  const double seconds = (preamble_symbols + 4.25 + payload_symbols) * symbol_seconds;
  return std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(seconds * 1000.0)));
}

void LoRaConfig::validate() const {
  if (bandwidth_khz <= 0.0) throw std::invalid_argument("LoRa bandwidth must be positive");
  if (spreading_factor < 5 || spreading_factor > 12)
    throw std::invalid_argument("LoRa spreading factor must be between 5 and 12");
  if (coding_rate_denominator < 5 || coding_rate_denominator > 8)
    throw std::invalid_argument("LoRa coding rate denominator must be between 5 and 8");
}

void SimRng::random(uint8_t* destination, size_t size) {
  for (size_t i = 0; i < size; ++i) destination[i] = static_cast<uint8_t>(engine_());
}

SimulatedMedium::SimulatedMedium(SimClock& clock, double interference, uint32_t seed)
    : clock_(clock), interference_(interference), random_(seed) {
  if (interference < 0.0 || interference > 1.0)
    throw std::invalid_argument("interference must be in the range 0..1");
}

size_t SimulatedMedium::addNode(const std::string& id) {
  if (indexes_.count(id)) throw std::runtime_error("duplicate repeater id: " + id);
  const size_t index = nodes_.size();
  indexes_[id] = index;
  nodes_.push_back(Node{id, {}});
  radios_.push_back(nullptr);
  return index;
}

void SimulatedMedium::addLink(const std::string& source, const std::string& target, bool undirected) {
  const auto source_it = indexes_.find(source);
  const auto target_it = indexes_.find(target);
  if (source_it == indexes_.end() || target_it == indexes_.end()) return;
  nodes_[source_it->second].outgoing.insert(target_it->second);
  if (undirected) nodes_[target_it->second].outgoing.insert(source_it->second);
}

void SimulatedMedium::attach(size_t node, SimulatedRadio* radio) {
  if (node >= radios_.size()) throw std::out_of_range("invalid radio node index");
  radios_[node] = radio;
}

bool SimulatedMedium::connected(size_t sender, size_t receiver) const {
  return sender < nodes_.size() && nodes_[sender].outgoing.count(receiver) != 0;
}

bool SimulatedMedium::overlapping(const Transmission& left, const Transmission& right) const {
  return left.started_at < right.ends_at && right.started_at < left.ends_at;
}

bool SimulatedMedium::sameChannel(const LoRaConfig& left, const LoRaConfig& right) const {
  constexpr double tolerance = 1e-6;
  return std::abs(left.frequency_mhz - right.frequency_mhz) < tolerance &&
         std::abs(left.bandwidth_khz - right.bandwidth_khz) < tolerance &&
         left.spreading_factor == right.spreading_factor;
}

bool SimulatedMedium::receiverCompatible(const LoRaConfig& transmission,
                                         const LoRaConfig& receiver) const {
  if (!sameChannel(transmission, receiver) ||
      transmission.explicit_header != receiver.explicit_header)
    return false;
  // Explicit LoRa headers carry the coding rate. In implicit-header mode the
  // receiver must already be configured with the transmitter's coding rate.
  return transmission.explicit_header ||
         transmission.coding_rate_denominator == receiver.coding_rate_denominator;
}

uint64_t SimulatedMedium::transmit(size_t sender, const uint8_t* bytes, size_t length,
                                   uint32_t airtime_ms, const LoRaConfig& config) {
  Transmission next{sender, clock_.now(), clock_.now() + airtime_ms,
                    std::vector<uint8_t>(bytes, bytes + length), config};
  for (const size_t receiver : nodes_[sender].outgoing) {
    if (radios_[receiver] != nullptr && receiverCompatible(config, radios_[receiver]->config()))
      next.observable_receivers.push_back(receiver);
  }
  transmissions_.push_back(std::move(next));
  ++stats_.transmissions;
  if (send_observer_) {
    const auto& transmission = transmissions_.back();
    send_observer_(nodes_[sender].id, transmission.started_at, transmission.bytes);
  }
  if (transmission_observer_) {
    const auto& transmission = transmissions_.back();
    for (const size_t receiver : transmission.observable_receivers) {
      transmission_observer_(nodes_[sender].id, nodes_[receiver].id, true,
                             transmission.started_at, airtime_ms, transmission.bytes);
    }
  }
  return transmissions_.back().ends_at;
}

bool SimulatedMedium::channelBusy(size_t receiver) const {
  if (receiver >= radios_.size() || radios_[receiver] == nullptr) return false;
  for (const auto& transmission : transmissions_) {
    if (!transmission.finalised && transmission.sender != receiver &&
        transmission.started_at < clock_.now() && connected(transmission.sender, receiver) &&
        sameChannel(transmission.config, radios_[receiver]->config())) return true;
  }
  return false;
}

void SimulatedMedium::finalise(Transmission& transmission) {
  transmission.finalised = true;
  if (transmission_observer_) {
    const uint32_t airtime_ms = static_cast<uint32_t>(transmission.ends_at - transmission.started_at);
    for (const size_t receiver : transmission.observable_receivers) {
      transmission_observer_(nodes_[transmission.sender].id, nodes_[receiver].id, false,
                             transmission.started_at, airtime_ms, transmission.bytes);
    }
  }

  std::uniform_real_distribution<double> distribution(0.0, 1.0);
  bool collided = false;
  for (const size_t receiver : nodes_[transmission.sender].outgoing) {
    if (radios_[receiver] == nullptr) continue;
    if (!receiverCompatible(transmission.config, radios_[receiver]->config())) {
      ++stats_.incompatible_drops;
      continue;
    }
    bool collision = false;
    for (const auto& other : transmissions_) {
      if (&other == &transmission || !overlapping(transmission, other)) continue;
      const bool receiver_is_transmitting = other.sender == receiver;
      const bool in_range_same_channel =
          connected(other.sender, receiver) && sameChannel(transmission.config, other.config);
      if (receiver_is_transmitting || in_range_same_channel) {
        collision = true;
        break;
      }
    }
    if (collision) {
      collided = true;
      ++stats_.collision_drops;
      continue;
    }
    if (distribution(random_) < interference_) {
      ++stats_.interference_drops;
      continue;
    }
    radios_[receiver]->deliver(transmission.bytes);
    ++stats_.delivered;
    if (delivery_observer_)
      delivery_observer_(nodes_[transmission.sender].id, nodes_[receiver].id,
                         clock_.now(), transmission.bytes);
  }
  if (collided) ++stats_.collided_transmissions;
}

void SimulatedMedium::advance() {
  // Indexing avoids invalidation concerns if a delivered packet immediately
  // causes a new transmission during the following repeater loop.
  for (size_t i = 0; i < transmissions_.size(); ++i) {
    if (!transmissions_[i].finalised && transmissions_[i].ends_at <= clock_.now())
      finalise(transmissions_[i]);
  }
  // Completed transmissions only need to remain while they overlap an active
  // one; this bounds memory during long simulations.
  uint64_t earliest_active_start = UINT64_MAX;
  for (const auto& transmission : transmissions_) {
    if (!transmission.finalised)
      earliest_active_start = std::min(earliest_active_start, transmission.started_at);
  }
  transmissions_.erase(std::remove_if(transmissions_.begin(), transmissions_.end(),
      [&](const Transmission& candidate) {
        return candidate.finalised && candidate.ends_at <= earliest_active_start;
      }), transmissions_.end());
}

SimulatedRadio::SimulatedRadio(SimulatedMedium& medium, SimClock& clock, LoRaConfig config, size_t node)
    : medium_(medium), clock_(clock), config_(config), node_(node) {
  config_.validate();
  medium_.attach(node_, this);
}

int SimulatedRadio::recvRaw(uint8_t* bytes, int size) {
  if (inbox_.empty() || !isInRecvMode()) return 0;
  Datagram datagram = std::move(inbox_.front());
  inbox_.pop_front();
  const int length = std::min<int>(size, static_cast<int>(datagram.bytes.size()));
  std::memcpy(bytes, datagram.bytes.data(), length);
  last_snr_ = datagram.snr;
  last_rssi_ = datagram.rssi;
  ++packets_received_;
  return length;
}

uint32_t SimulatedRadio::getEstAirtimeFor(int length) {
  return config_.airtimeMillis(static_cast<size_t>(std::max(0, length)));
}

float SimulatedRadio::packetScore(float snr, int packet_length) {
  static const float thresholds[] = {-7.5f, -10.0f, -12.5f, -15.0f, -17.5f, -20.0f};
  const size_t index = static_cast<size_t>(std::clamp<int>(config_.spreading_factor, 7, 12) - 7);
  if (snr < thresholds[index]) return 0.0f;
  return std::clamp(((snr - thresholds[index]) / 10.0f) * (1.0f - packet_length / 256.0f), 0.0f, 1.0f);
}

bool SimulatedRadio::startSendRaw(const uint8_t* bytes, int length) {
  if (transmitting_until_ > clock_.now()) return false;
  transmitting_until_ = medium_.transmit(
      node_, bytes, static_cast<size_t>(length), getEstAirtimeFor(length), config_);
  ++packets_sent_;
  return true;
}

bool SimulatedRadio::isSendComplete() { return transmitting_until_ != 0 && clock_.now() >= transmitting_until_; }

void SimulatedRadio::onSendFinished() { transmitting_until_ = 0; }

bool SimulatedRadio::isInRecvMode() const { return transmitting_until_ == 0 || clock_.now() >= transmitting_until_; }

bool SimulatedRadio::isReceiving() { return isInRecvMode() && medium_.channelBusy(node_); }

void SimulatedRadio::deliver(const std::vector<uint8_t>& bytes, float snr, float rssi) {
  inbox_.push_back(Datagram{bytes, snr, rssi});
}

void SimulatedRadio::configure(const LoRaConfig& config) {
  config.validate();
  config_ = config;
}

void SimulatedRadio::setParams(float frequency, float bandwidth, uint8_t spreading_factor,
                               uint8_t coding_rate) {
  config_.frequency_mhz = frequency;
  config_.bandwidth_khz = bandwidth;
  config_.spreading_factor = spreading_factor;
  config_.coding_rate_denominator = coding_rate;
  config_.preamble_symbols = spreading_factor <= 8 ? 32 : 16;
  config_.validate();
}

namespace {
void checkSqlite(int result, sqlite3* database, const std::string& context) {
  if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW)
    throw std::runtime_error(context + ": " + sqlite3_errmsg(database));
}
}  // namespace

DatabaseTopology loadTopology(const std::string& path, size_t node_limit) {
  sqlite3* raw_database = nullptr;
  if (sqlite3_open_v2(path.c_str(), &raw_database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    const std::string error = raw_database ? sqlite3_errmsg(raw_database) : "unable to allocate SQLite handle";
    if (raw_database) sqlite3_close(raw_database);
    throw std::runtime_error("cannot open topology database: " + error);
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw_database, sqlite3_close);
  DatabaseTopology topology;

  sqlite3_stmt* raw_statement = nullptr;
  checkSqlite(sqlite3_prepare_v2(database.get(),
      "SELECT repeater_id, COALESCE(public_key, '') FROM repeaters ORDER BY repeater_id", -1,
      &raw_statement, nullptr), database.get(), "prepare repeater query");
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw_statement, sqlite3_finalize);
  while (sqlite3_step(statement.get()) == SQLITE_ROW && (!node_limit || topology.nodes.size() < node_limit)) {
    topology.nodes.push_back({reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0)),
                              reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1))});
  }

  std::unordered_set<std::string> selected;
  for (const auto& node : topology.nodes) selected.insert(node.id);
  raw_statement = nullptr;
  checkSqlite(sqlite3_prepare_v2(database.get(),
      "SELECT source_id, target_id FROM directed_links ORDER BY source_id, target_id", -1,
      &raw_statement, nullptr), database.get(), "prepare link query");
  statement.reset(raw_statement);
  while (sqlite3_step(statement.get()) == SQLITE_ROW) {
    std::string source = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
    std::string target = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
    if (selected.count(source) && selected.count(target)) topology.links.push_back({source, target});
  }
  return topology;
}

}  // namespace simulator
