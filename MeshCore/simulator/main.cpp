#include "Simulator.h"
#include "CompanionEndpoint.h"
#include "RepeaterFirmware.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>

static_assert(MAX_CONTACTS == 100,
              "the host build must use the companion firmware contact-table size");

namespace {

struct Injection {
  std::string node;
  uint64_t at_ms = 0;
  bool done = false;
};

struct CompanionAttachment {
  std::string node;
  uint16_t port;
};

struct Options {
  std::string database;
  size_t node_limit = 0;
  uint64_t duration_ms = 10'000;
  uint32_t tick_ms = 1;
  uint32_t seed = 1;
  double interference = 0.0;
  // A row records the order in which an adjacency was observed, not a
  // one-way radio link. LoRa reachability is symmetric in the simulator.
  bool undirected = true;
  bool self_test = false;
  bool preamble_set = false;
  bool realtime = false;
  std::string companion_bind = "127.0.0.1";
  std::string companion_storage;
  simulator::LoRaConfig radio;
  std::vector<Injection> injections;
  std::vector<CompanionAttachment> companions;
};

volatile std::sig_atomic_t stop_requested = 0;

void requestStop(int) { stop_requested = 1; }

uint64_t parseUnsigned(const char* value, const char* name) {
  char* end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  if (!value[0] || (end && *end)) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
  return parsed;
}

double parseDouble(const char* value, const char* name) {
  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (!value[0] || (end && *end)) throw std::invalid_argument(std::string("invalid ") + name + ": " + value);
  return parsed;
}

void usage(std::ostream& output) {
  output << "Usage: meshcore-repeater-sim --database PATH [options]\n"
         << "  --duration-ms N       simulated duration (default 10000)\n"
         << "  --nodes N             limit database nodes; 0 means all\n"
         << "  --tick-ms N           scheduler resolution (default 1)\n"
         << "  --inject ID[@MS]      inject a three-byte flood advert; repeatable\n"
         << "  --interference P      independent per-link drop probability 0..1\n"
         << "  --links MODE          undirected (default) or directed override\n"
         << "  --frequency MHZ       LoRa frequency metadata (default 915)\n"
         << "  --bandwidth KHZ       LoRa bandwidth (default 250)\n"
         << "  --sf N                spreading factor 5..12 (default 10)\n"
         << "  --cr N                coding rate denominator 5..8 (default 5)\n"
         << "  --preamble N          preamble symbols (default 16)\n"
         << "  --seed N              deterministic random seed\n"
         << "  --companion ID@PORT   attach a TCP companion endpoint; repeatable (max 2)\n"
         << "  --companion-bind IP   TCP bind address (default 127.0.0.1)\n"
         << "  --companion-storage D persistent companion filesystem directory\n"
         << "  --realtime            pace virtual milliseconds to wall-clock time\n"
         << "  --self-test           run radio/airtime checks without SQLite\n";
}

Options parseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto value = [&](const char* name) -> const char* {
      if (++i >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
      return argv[i];
    };
    if (argument == "--database") options.database = value("--database");
    else if (argument == "--duration-ms") options.duration_ms = parseUnsigned(value("--duration-ms"), "duration");
    else if (argument == "--nodes") options.node_limit = parseUnsigned(value("--nodes"), "node count");
    else if (argument == "--tick-ms") options.tick_ms = parseUnsigned(value("--tick-ms"), "tick");
    else if (argument == "--seed") options.seed = parseUnsigned(value("--seed"), "seed");
    else if (argument == "--interference") options.interference = parseDouble(value("--interference"), "interference");
    else if (argument == "--frequency") options.radio.frequency_mhz = parseDouble(value("--frequency"), "frequency");
    else if (argument == "--bandwidth") options.radio.bandwidth_khz = parseDouble(value("--bandwidth"), "bandwidth");
    else if (argument == "--sf") options.radio.spreading_factor = parseUnsigned(value("--sf"), "spreading factor");
    else if (argument == "--cr") options.radio.coding_rate_denominator = parseUnsigned(value("--cr"), "coding rate");
    else if (argument == "--preamble") {
      options.radio.preamble_symbols = parseUnsigned(value("--preamble"), "preamble");
      options.preamble_set = true;
    }
    else if (argument == "--links") {
      const std::string mode = value("--links");
      if (mode != "directed" && mode != "undirected") throw std::invalid_argument("links must be directed or undirected");
      options.undirected = mode == "undirected";
    } else if (argument == "--inject") {
      std::string injection = value("--inject");
      const size_t delimiter = injection.find('@');
      Injection parsed;
      parsed.node = injection.substr(0, delimiter);
      if (delimiter != std::string::npos)
        parsed.at_ms = parseUnsigned(injection.c_str() + delimiter + 1, "injection time");
      options.injections.push_back(parsed);
    } else if (argument == "--companion") {
      const std::string attachment = value("--companion");
      const size_t delimiter = attachment.rfind('@');
      if (delimiter == std::string::npos || delimiter == 0)
        throw std::invalid_argument("companion must use ID@PORT");
      const auto parsed_port = parseUnsigned(attachment.c_str() + delimiter + 1, "companion port");
      if (parsed_port == 0 || parsed_port > 65535)
        throw std::invalid_argument("companion port must be between 1 and 65535");
      options.companions.push_back({attachment.substr(0, delimiter), static_cast<uint16_t>(parsed_port)});
    } else if (argument == "--companion-bind") options.companion_bind = value("--companion-bind");
    else if (argument == "--companion-storage") options.companion_storage = value("--companion-storage");
    else if (argument == "--realtime") options.realtime = true;
    else if (argument == "--self-test") options.self_test = true;
    else if (argument == "--help" || argument == "-h") { usage(std::cout); std::exit(0); }
    else throw std::invalid_argument("unknown option: " + argument);
  }
  if (!options.self_test && options.database.empty()) throw std::invalid_argument("--database is required");
  if (options.tick_ms == 0) throw std::invalid_argument("--tick-ms must be positive");
  if (options.companions.size() > 2) throw std::invalid_argument("at most two companion endpoints are supported");
  if (options.companions.size() == 2 && options.companions[0].port == options.companions[1].port)
    throw std::invalid_argument("companion ports must be distinct");
  if (!options.companions.empty()) options.realtime = true;
  if (!options.companions.empty() && options.companion_storage.empty())
    options.companion_storage = options.database + ".companions";
  if (!options.preamble_set)
    options.radio.preamble_symbols = options.radio.spreading_factor <= 8 ? 32 : 16;
  options.radio.validate();
  return options;
}

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(std::string("self-test failed: ") + message);
}

bool isRepeaterId(const std::string& value) {
  return value.size() == 6 && std::all_of(value.begin(), value.end(), [](const char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'A' && character <= 'F');
  });
}

void connectCompanionNeighborhood(simulator::SimulatedMedium& medium,
                                  const std::string& endpoint_id,
                                  const std::string& selected_repeater) {
  const auto selected = std::find_if(
      medium.nodes().begin(), medium.nodes().end(),
      [&](const auto& node) { return node.id == selected_repeater; });
  if (selected == medium.nodes().end()) return;

  std::vector<std::string> audible_repeaters{selected_repeater};
  for (const size_t neighbor_index : selected->outgoing) {
    const std::string& neighbor_id = medium.nodes()[neighbor_index].id;
    if (isRepeaterId(neighbor_id)) audible_repeaters.push_back(neighbor_id);
  }
  for (const auto& repeater_id : audible_repeaters) {
    medium.addLink(endpoint_id, repeater_id, false);
    medium.addLink(repeater_id, endpoint_id, false);
  }
}

uint64_t packetEventId(const std::vector<uint8_t>& raw) {
  if (raw.size() < 2) return 0;
  const size_t path_offset = 1 + (((raw[0] & 0x03) == 0 || (raw[0] & 0x03) == 3) ? 4 : 0);
  if (path_offset >= raw.size()) return 0;
  const uint8_t path_length = raw[path_offset];
  const size_t hash_size = (path_length >> 6) + 1;
  const size_t hash_count = path_length & 0x3f;
  const size_t payload_offset = path_offset + 1 + hash_size * hash_count;
  if (hash_size > 3 || payload_offset > raw.size()) return 0;
  uint64_t hash = 1469598103934665603ULL;
  hash = (hash ^ ((raw[0] >> 2) & 0x0f)) * 1099511628211ULL;
  for (size_t index = payload_offset; index < raw.size(); ++index)
    hash = (hash ^ raw[index]) * 1099511628211ULL;
  return hash;
}

bool isIntendedDirectReceiver(const std::vector<uint8_t>& raw, const std::string& receiver) {
  if (raw.size() < 2) return false;
  const uint8_t route_type = raw[0] & 0x03;
  if (route_type != 2 && route_type != 3) return true;
  const size_t path_offset = 1 + (route_type == 3 ? 4 : 0);
  if (path_offset >= raw.size()) return false;
  const uint8_t path_length = raw[path_offset];
  const size_t hash_size = (path_length >> 6) + 1;
  const size_t hash_count = path_length & 0x3f;
  if (hash_count == 0 || hash_size > 3 || path_offset + 1 + hash_size > raw.size()) return false;
  for (size_t index = 0; index < hash_size; ++index) {
    const char high = receiver[index * 2];
    const char low = receiver[index * 2 + 1];
    const auto nibble = [](const char character) -> uint8_t {
      return character <= '9' ? static_cast<uint8_t>(character - '0')
                              : static_cast<uint8_t>(character - 'A' + 10);
    };
    if (raw[path_offset + 1 + index] != static_cast<uint8_t>((nibble(high) << 4) | nibble(low)))
      return false;
  }
  return true;
}

void selfTest() {
  require(Options{}.undirected,
          "database adjacency observations must be bidirectional by default");

  const std::vector<uint8_t> route_variant_a{
      0x14, 0, 0, 0, 0, 0x01, 0xAA, 0xDE, 0xAD};
  const std::vector<uint8_t> route_variant_b{
      0x14, 0, 0, 0, 0, 0x01, 0xBB, 0xDE, 0xAD};
  const std::vector<uint8_t> payload_variant{
      0x14, 0, 0, 0, 0, 0x01, 0xAA, 0xDE, 0xAE};
  require(packetEventId(route_variant_a) == packetEventId(route_variant_b),
          "packet color identity must ignore mutable route path bytes");
  require(packetEventId(route_variant_a) != packetEventId(payload_variant),
          "packet color identity must change when packet payload data changes");

  simulator::LoRaConfig config;
  require(config.airtimeMillis(102) == 546, "SF10/BW250/CR4:5 airtime should be 546 ms for 102 bytes");
  require(config.airtimeMillis(200) > config.airtimeMillis(100), "airtime must grow with payload size");

  simulator::SimClock clock;
  simulator::SimulatedMedium medium(clock, 0.0, 7);
  const size_t a = medium.addNode("AAAAAA");
  const size_t b = medium.addNode("BBBBBB");
  const size_t c = medium.addNode("CCCCCC");
  medium.addLink("AAAAAA", "BBBBBB", false);
  medium.addLink("BBBBBB", "AAAAAA", false);
  medium.addLink("AAAAAA", "CCCCCC", false);
  medium.addLink("BBBBBB", "CCCCCC", false);
  simulator::SimulatedRadio radio_a(medium, clock, config, a);
  simulator::SimulatedRadio radio_b(medium, clock, config, b);
  simulator::SimulatedRadio radio_c(medium, clock, config, c);
  const uint8_t packet[] = {1, 0, 42};
  require(radio_a.startSendRaw(packet, sizeof(packet)), "first simultaneous transmission should start");
  require(radio_b.startSendRaw(packet, sizeof(packet)), "second simultaneous transmission should start");
  clock.set(config.airtimeMillis(sizeof(packet)));
  medium.advance();
  uint8_t output[8];
  require(radio_c.recvRaw(output, sizeof(output)) == 0, "in-range simultaneous transmissions must collide");
  require(medium.stats().collided_transmissions == 2, "both colliding transmissions must be marked");

  simulator::SimClock hidden_clock;
  simulator::SimulatedMedium hidden_medium(hidden_clock, 0.0, 8);
  const size_t hidden_a = hidden_medium.addNode("AAAAAA");
  const size_t hidden_b = hidden_medium.addNode("BBBBBB");
  const size_t hidden_c = hidden_medium.addNode("CCCCCC");
  hidden_medium.addLink("AAAAAA", "CCCCCC", false);
  hidden_medium.addLink("BBBBBB", "CCCCCC", false);
  simulator::SimulatedRadio hidden_radio_a(hidden_medium, hidden_clock, config, hidden_a);
  simulator::SimulatedRadio hidden_radio_b(hidden_medium, hidden_clock, config, hidden_b);
  simulator::SimulatedRadio hidden_radio_c(hidden_medium, hidden_clock, config, hidden_c);
  hidden_radio_a.startSendRaw(packet, sizeof(packet));
  hidden_radio_b.startSendRaw(packet, sizeof(packet));
  hidden_clock.set(config.airtimeMillis(sizeof(packet)));
  hidden_medium.advance();
  require(hidden_radio_c.recvRaw(output, sizeof(output)) == 0, "hidden terminals must collide at a common receiver");
  require(hidden_medium.stats().collision_drops == 2, "both hidden-terminal receptions must be dropped");

  simulator::SimClock local_collision_clock;
  simulator::SimulatedMedium local_collision_medium(local_collision_clock, 0.0, 13);
  const size_t local_a = local_collision_medium.addNode("AAAAAA");
  const size_t local_b = local_collision_medium.addNode("BBBBBB");
  const size_t local_c = local_collision_medium.addNode("CCCCCC");
  const size_t local_d = local_collision_medium.addNode("DDDDDD");
  local_collision_medium.addLink("AAAAAA", "BBBBBB", true);
  local_collision_medium.addLink("AAAAAA", "CCCCCC", false);
  local_collision_medium.addLink("BBBBBB", "DDDDDD", false);
  simulator::SimulatedRadio local_radio_a(
      local_collision_medium, local_collision_clock, config, local_a);
  simulator::SimulatedRadio local_radio_b(
      local_collision_medium, local_collision_clock, config, local_b);
  simulator::SimulatedRadio local_radio_c(
      local_collision_medium, local_collision_clock, config, local_c);
  simulator::SimulatedRadio local_radio_d(
      local_collision_medium, local_collision_clock, config, local_d);
  local_radio_a.startSendRaw(packet, sizeof(packet));
  local_radio_b.startSendRaw(packet, sizeof(packet));
  local_collision_clock.set(config.airtimeMillis(sizeof(packet)));
  local_collision_medium.advance();
  require(local_radio_c.recvRaw(output, sizeof(output)) == sizeof(packet) &&
              local_radio_d.recvRaw(output, sizeof(output)) == sizeof(packet),
          "a collision must not drop packets at receivers which hear only one sender");
  require(local_collision_medium.stats().delivered == 2,
          "receiver-specific collision handling must preserve exclusive deliveries");

  simulator::SimClock compatibility_clock;
  simulator::SimulatedMedium compatibility_medium(compatibility_clock, 0.0, 14);
  const size_t compatibility_a = compatibility_medium.addNode("AAAAAA");
  const size_t compatibility_b = compatibility_medium.addNode("BBBBBB");
  compatibility_medium.addLink("AAAAAA", "BBBBBB", false);
  simulator::LoRaConfig incompatible_config = config;
  incompatible_config.spreading_factor = 7;
  incompatible_config.preamble_symbols = 32;
  simulator::SimulatedRadio compatibility_radio_a(
      compatibility_medium, compatibility_clock, config, compatibility_a);
  simulator::SimulatedRadio compatibility_radio_b(
      compatibility_medium, compatibility_clock, incompatible_config, compatibility_b);
  compatibility_radio_a.startSendRaw(packet, sizeof(packet));
  compatibility_clock.set(1);
  require(!compatibility_radio_b.isReceiving(),
          "a receiver on a different LoRa channel must not report channel activity");
  compatibility_clock.set(config.airtimeMillis(sizeof(packet)));
  compatibility_medium.advance();
  require(compatibility_radio_b.recvRaw(output, sizeof(output)) == 0 &&
              compatibility_medium.stats().incompatible_drops == 1,
          "a receiver with incompatible LoRa parameters must not decode the packet");
  compatibility_radio_a.onSendFinished();
  compatibility_radio_b.configure(config);
  compatibility_radio_a.startSendRaw(packet, sizeof(packet));
  compatibility_clock.set(compatibility_clock.now() + config.airtimeMillis(sizeof(packet)));
  compatibility_medium.advance();
  require(compatibility_radio_b.recvRaw(output, sizeof(output)) == sizeof(packet),
          "matching LoRa parameters must restore delivery");

  simulator::SimClock observed_clock;
  simulator::SimulatedMedium observed_medium(observed_clock, 0.0, 10);
  const size_t observed_a = observed_medium.addNode("AAAAAA");
  const size_t observed_b = observed_medium.addNode("BBBBBB");
  observed_medium.addLink("AAAAAA", "BBBBBB", false);
  simulator::SimulatedRadio observed_radio_a(observed_medium, observed_clock, config, observed_a);
  simulator::SimulatedRadio observed_radio_b(observed_medium, observed_clock, config, observed_b);
  std::string observed_edge;
  std::vector<bool> observed_transmission_states;
  uint32_t observed_duration = 0;
  size_t observed_packet_length = 0;
  size_t observed_sends = 0;
  observed_medium.setSendObserver(
      [&](const std::string& source, uint64_t, const std::vector<uint8_t>& bytes) {
        require(source == "AAAAAA", "send events must identify their transmitter");
        observed_packet_length = bytes.size();
        ++observed_sends;
      });
  observed_medium.setDeliveryObserver(
      [&](const std::string& source, const std::string& target, uint64_t,
          const std::vector<uint8_t>&) {
        observed_edge = source + ">" + target;
      });
  observed_medium.setTransmissionObserver(
      [&](const std::string& source, const std::string& target, bool active, uint64_t,
          uint32_t duration_ms, const std::vector<uint8_t>&) {
        require(source == "AAAAAA" && target == "BBBBBB",
                "transmission events must identify their directed edge");
        observed_transmission_states.push_back(active);
        observed_duration = duration_ms;
      });
  observed_radio_a.startSendRaw(packet, sizeof(packet));
  require(observed_sends == 1 && observed_packet_length == sizeof(packet),
          "each physical transmission must emit its packet length once");
  require(observed_transmission_states == std::vector<bool>{true} &&
              observed_duration == config.airtimeMillis(sizeof(packet)),
          "transmission start must report the calculated on-air duration");
  observed_clock.set(config.airtimeMillis(sizeof(packet)));
  observed_medium.advance();
  require(observed_edge == "AAAAAA>BBBBBB", "successful delivery must emit its directed hop");
  require(observed_transmission_states == (std::vector<bool>{true, false}),
          "transmission end must be emitted when the on-air interval completes");

  simulator::SimClock neighborhood_clock;
  simulator::SimulatedMedium neighborhood_medium(neighborhood_clock, 0.0, 12);
  const size_t selected = neighborhood_medium.addNode("111111");
  const size_t direct_a = neighborhood_medium.addNode("222222");
  const size_t direct_b = neighborhood_medium.addNode("333333");
  const size_t two_hops_away = neighborhood_medium.addNode("444444");
  const size_t endpoint = neighborhood_medium.addNode("tcp-companion-5000");
  simulator::SimulatedRadio selected_radio(
      neighborhood_medium, neighborhood_clock, config, selected);
  simulator::SimulatedRadio direct_a_radio(
      neighborhood_medium, neighborhood_clock, config, direct_a);
  simulator::SimulatedRadio direct_b_radio(
      neighborhood_medium, neighborhood_clock, config, direct_b);
  simulator::SimulatedRadio two_hops_radio(
      neighborhood_medium, neighborhood_clock, config, two_hops_away);
  simulator::SimulatedRadio endpoint_radio(
      neighborhood_medium, neighborhood_clock, config, endpoint);
  neighborhood_medium.addLink("111111", "222222", true);
  neighborhood_medium.addLink("111111", "333333", true);
  neighborhood_medium.addLink("222222", "444444", true);
  connectCompanionNeighborhood(neighborhood_medium, "tcp-companion-5000", "111111");
  require(neighborhood_medium.nodes()[endpoint].outgoing.count(selected) == 1 &&
              neighborhood_medium.nodes()[endpoint].outgoing.count(direct_a) == 1 &&
              neighborhood_medium.nodes()[endpoint].outgoing.count(direct_b) == 1,
          "companion must hear its selected repeater and all direct neighbors");
  require(neighborhood_medium.nodes()[endpoint].outgoing.count(two_hops_away) == 0,
          "companion must not hear repeaters two hops from its selection");
  require(neighborhood_medium.nodes()[selected].outgoing.count(endpoint) == 1 &&
              neighborhood_medium.nodes()[direct_a].outgoing.count(endpoint) == 1 &&
              neighborhood_medium.nodes()[direct_b].outgoing.count(endpoint) == 1,
          "selected repeater neighborhood must hear the companion");
  require(endpoint_radio.startSendRaw(packet, sizeof(packet)),
          "companion neighborhood test transmission should start");
  neighborhood_clock.set(config.airtimeMillis(sizeof(packet)));
  neighborhood_medium.advance();
  require(selected_radio.recvRaw(output, sizeof(output)) == sizeof(packet) &&
              direct_a_radio.recvRaw(output, sizeof(output)) == sizeof(packet) &&
              direct_b_radio.recvRaw(output, sizeof(output)) == sizeof(packet),
          "selected repeater and direct neighbors must receive companion broadcasts");
  require(two_hops_radio.recvRaw(output, sizeof(output)) == 0,
          "two-hop repeaters must not receive companion broadcasts directly");
  endpoint_radio.onSendFinished();
  require(direct_a_radio.startSendRaw(packet, sizeof(packet)),
          "direct-neighbor return transmission should start");
  neighborhood_clock.set(neighborhood_clock.now() + config.airtimeMillis(sizeof(packet)));
  neighborhood_medium.advance();
  require(endpoint_radio.recvRaw(output, sizeof(output)) == sizeof(packet),
          "companion must receive broadcasts from direct neighbors");

  simulator::SimClock loss_clock;
  simulator::SimulatedMedium loss_medium(loss_clock, 1.0, 9);
  const size_t loss_a = loss_medium.addNode("AAAAAA");
  const size_t loss_b = loss_medium.addNode("BBBBBB");
  loss_medium.addLink("AAAAAA", "BBBBBB", false);
  simulator::SimulatedRadio loss_radio_a(loss_medium, loss_clock, config, loss_a);
  simulator::SimulatedRadio loss_radio_b(loss_medium, loss_clock, config, loss_b);
  uint32_t observed_deliveries = 0;
  loss_medium.setDeliveryObserver([&](const std::string&, const std::string&, uint64_t,
                                      const std::vector<uint8_t>&) {
    ++observed_deliveries;
  });
  loss_radio_a.startSendRaw(packet, sizeof(packet));
  loss_clock.set(config.airtimeMillis(sizeof(packet)));
  loss_medium.advance();
  require(loss_radio_b.recvRaw(output, sizeof(output)) == 0, "100% interference must drop delivery");
  require(loss_medium.stats().interference_drops == 1, "interference drop must be counted");
  require(observed_deliveries == 0, "dropped packets must not emit delivery events");

  simulator::SimClock firmware_clock;
  simulator::SimulatedMedium firmware_medium(firmware_clock, 0.0, 11);
  const size_t firmware_node = firmware_medium.addNode("ABC123");
  simulator::LoRaConfig firmware_config = config;
  firmware_config.spreading_factor = 7;
  firmware_config.preamble_symbols = 24;
  simulator::RepeaterFirmware firmware_repeater(
      simulator::DatabaseNode{"ABC123", ""}, firmware_node, firmware_medium,
      firmware_clock, firmware_config, 12);
  firmware_repeater.begin();
  require(firmware_repeater.identityMatchesDatabasePrefix(),
          "real repeater identity must use the database prefix");
  require(firmware_repeater.radioConfig().spreading_factor == 7 &&
              firmware_repeater.radioConfig().preamble_symbols == 24,
          "simulator radio options must survive firmware startup");
  firmware_repeater.injectAdvert();
  firmware_repeater.loop();
  firmware_clock.set(1);
  firmware_medium.advance();
  firmware_repeater.loop();
  require(firmware_medium.stats().transmissions == 1,
          "real simple_repeater firmware must transmit through the simulated radio");
  firmware_clock.set(1 + firmware_config.airtimeMillis(255));
  firmware_medium.advance();
  firmware_repeater.loop();
  firmware_clock.set(120'001);
  firmware_repeater.loop();
  require(firmware_medium.stats().transmissions == 1,
          "disabling repeater adverts must cancel timers scheduled during startup");
  std::cout << "self-test: ok\n";
}

struct RepeaterInstance {
  simulator::RepeaterFirmware repeater;

  RepeaterInstance(const simulator::DatabaseNode& node, size_t index,
                   simulator::SimulatedMedium& medium, simulator::SimClock& clock,
                   const simulator::LoRaConfig& config, uint32_t seed)
      : repeater(node, index, medium, clock, config, seed) {}
};

void run(const Options& options) {
  const auto topology = simulator::loadTopology(options.database, options.node_limit);
  if (topology.nodes.empty()) throw std::runtime_error("database contains no repeaters");
  simulator::SimClock clock;
  simulator::SimulatedMedium medium(clock, options.interference, options.seed ^ 0xa5a5a5a5U);
  for (const auto& node : topology.nodes) medium.addNode(node.id);
  for (const auto& link : topology.links) medium.addLink(link.source, link.target, options.undirected);
  if (!options.companions.empty()) {
    medium.setSendObserver([](const std::string& source, uint64_t started_at,
                              const std::vector<uint8_t>& bytes) {
      if (!isRepeaterId(source)) return;
      std::cout << "PACKET_SENT "
                << "{\"source\":\"" << source << "\",\"packet\":\"" << std::hex
                << packetEventId(bytes) << std::dec << "\",\"transmission\":" << started_at
                << ",\"packet_length\":" << bytes.size() << "}" << std::endl;
    });
    medium.setTransmissionObserver([](const std::string& source, const std::string& target,
                                      bool active, uint64_t started_at, uint32_t duration_ms,
                                      const std::vector<uint8_t>& bytes) {
      if (!isRepeaterId(source) || !isRepeaterId(target)) return;
      if (!isIntendedDirectReceiver(bytes, target)) return;
      std::cout << (active ? "ROUTE_START " : "ROUTE_END ")
                << "{\"source\":\"" << source << "\",\"target\":\"" << target
                << "\",\"packet\":\"" << std::hex << packetEventId(bytes) << std::dec
                << "\",\"transmission\":" << started_at;
      if (active) std::cout << ",\"duration_ms\":" << duration_ms;
      std::cout << "}" << std::endl;
    });
  }

  std::vector<std::unique_ptr<simulator::CompanionEndpoint>> companions;
  for (const auto& attachment : options.companions) {
    if (std::none_of(topology.nodes.begin(), topology.nodes.end(),
                     [&](const auto& node) { return node.id == attachment.node; }))
      throw std::runtime_error("companion attachment node is not loaded: " + attachment.node);
    const std::string endpoint_id = "tcp-companion-" + std::to_string(attachment.port);
    const size_t endpoint_index = medium.addNode(endpoint_id);
    connectCompanionNeighborhood(medium, endpoint_id, attachment.node);
    const auto storage_image =
        (std::filesystem::path(options.companion_storage) /
         ("companion-" + std::to_string(attachment.port) + ".fs"))
            .string();
    companions.push_back(std::make_unique<simulator::CompanionEndpoint>(
        attachment.node, options.companion_bind, attachment.port, medium, clock,
        options.radio, endpoint_index, storage_image));
  }

  std::vector<std::unique_ptr<RepeaterInstance>> repeaters;
  std::unordered_map<std::string, simulator::RepeaterFirmware*> by_id;
  repeaters.reserve(topology.nodes.size());
  for (size_t i = 0; i < topology.nodes.size(); ++i) {
    repeaters.push_back(std::make_unique<RepeaterInstance>(topology.nodes[i], i, medium, clock,
                                                           options.radio, options.seed + static_cast<uint32_t>(i + 1)));
    by_id[topology.nodes[i].id] = &repeaters.back()->repeater;
    repeaters.back()->repeater.begin();
  }
  for (auto& companion : companions) {
    companion->start();
    std::cout << "companion repeater=" << companion->repeaterId() << " listen="
              << options.companion_bind << ':' << companion->port() << '\n';
  }

  std::vector<Injection> injections = options.injections;
  if (injections.empty() && companions.empty()) {
    const auto source = std::find_if(medium.nodes().begin(), medium.nodes().end(),
                                     [](const auto& node) { return !node.outgoing.empty(); });
    injections.push_back({source == medium.nodes().end() ? topology.nodes.front().id : source->id, 0, false});
  }

  const auto wall_start = std::chrono::steady_clock::now();
  for (uint64_t now = 0; !stop_requested && (options.duration_ms == 0 || now <= options.duration_ms);
       now += options.tick_ms) {
    if (options.realtime)
      std::this_thread::sleep_until(wall_start + std::chrono::milliseconds(now));
    clock.set(now);
    medium.advance();
    for (auto& injection : injections) {
      if (!injection.done && injection.at_ms <= now) {
        const auto found = by_id.find(injection.node);
        if (found == by_id.end()) throw std::runtime_error("injection node is not loaded: " + injection.node);
        found->second->injectAdvert();
        injection.done = true;
      }
    }
    for (auto& repeater : repeaters) repeater->repeater.loop();
    for (auto& companion : companions) companion->pump();
  }

  for (auto& companion : companions) companion->stop();

  uint64_t adverts_received = 0;
  uint64_t mesh_sent = 0;
  uint64_t mesh_received = 0;
  for (const auto& instance : repeaters) {
    mesh_sent += instance->repeater.sentFlood() + instance->repeater.sentDirect();
    mesh_received += instance->repeater.receivedFlood() + instance->repeater.receivedDirect();
  }
  const auto& stats = medium.stats();
  std::cout << "nodes=" << topology.nodes.size() << " links=" << topology.links.size()
            << " link_mode=" << (options.undirected ? "undirected" : "directed") << '\n'
            << "radio=" << options.radio.frequency_mhz << "MHz BW" << options.radio.bandwidth_khz
            << "kHz SF" << static_cast<int>(options.radio.spreading_factor) << " CR4/"
            << static_cast<int>(options.radio.coding_rate_denominator)
            << " advert_airtime_ms=" << options.radio.airtimeMillis(102) << '\n'
            << "transmissions=" << stats.transmissions << " delivered_datagrams=" << stats.delivered
            << " mesh_tx=" << mesh_sent << " mesh_rx=" << mesh_received
            << " adverts_received=" << adverts_received << '\n'
            << "collided_transmissions=" << stats.collided_transmissions
            << " collision_drops=" << stats.collision_drops
            << " incompatible_drops=" << stats.incompatible_drops
            << " interference_drops=" << stats.interference_drops << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    const Options options = parseOptions(argc, argv);
    if (options.self_test) selfTest();
    else run(options);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
