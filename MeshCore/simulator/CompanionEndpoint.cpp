#define MyMesh CompanionFirmwareMyMesh
#define NodePrefs CompanionFirmwareNodePrefs
#define board companion_firmware_board
#define radio_driver companion_firmware_radio_driver
#define rtc_clock companion_firmware_rtc_clock
#define sensors companion_firmware_sensors
#define radio_new_identity companion_firmware_new_identity
#include "../examples/companion_radio/MyMesh.h"
#undef MyMesh
#undef NodePrefs
#undef board
#undef radio_driver
#undef rtc_clock
#undef sensors
#undef radio_new_identity

#include "CompanionEndpoint.h"
#include "HostPlatform.h"
#include "TcpSerialInterface.h"

#include <stdexcept>

HostBoard companion_firmware_board;
HostRadioDriver companion_firmware_radio_driver;
HostRTCClockProxy companion_firmware_rtc_clock;
SensorManager companion_firmware_sensors;

mesh::LocalIdentity companion_firmware_new_identity() {
  auto* context = simulator::activeFirmwareContext();
  if (!context || !context->rng)
    throw std::runtime_error("companion identity generation requires an active firmware context");
  return mesh::LocalIdentity(context->rng);
}

namespace simulator {

class CompanionEndpoint::Impl {
public:
  std::string repeater_id;
  uint16_t port;
  LoRaConfig startup_config;
  SimpleMeshTables tables;
  SimRng rng;
  SimulatedRadio radio;
  fs::SPIFFSFS filesystem;
  FirmwareContext context;
  DataStore store;
  TcpSerialInterface serial;
  CompanionFirmwareMyMesh mesh;
  double latitude = 0;
  double longitude = 0;
  bool started = false;

  Impl(const std::string& attached_repeater, const std::string& bind_address,
       uint16_t listen_port, SimulatedMedium& medium, SimClock& clock,
       const LoRaConfig& config, size_t node_index, const std::string& storage_image)
      : repeater_id(attached_repeater), port(listen_port), startup_config(config),
        rng(0x434f4d50U ^ listen_port),
        radio(medium, clock, config, node_index),
        context{&radio, &clock, &rng, &filesystem, false}, store(filesystem, clock),
        serial(bind_address, listen_port), mesh(radio, rng, clock, tables, store) {
    if (!filesystem.attachImage(storage_image))
      throw std::runtime_error("cannot load companion storage image: " + storage_image);
  }

  void enterSensorContext() {
    companion_firmware_sensors.node_lat = latitude;
    companion_firmware_sensors.node_lon = longitude;
  }
  void leaveSensorContext() {
    latitude = companion_firmware_sensors.node_lat;
    longitude = companion_firmware_sensors.node_lon;
  }
};

CompanionEndpoint::CompanionEndpoint(const std::string& repeater_id,
                                     const std::string& bind_address, uint16_t port,
                                     SimulatedMedium& medium, SimClock& clock,
                                     const LoRaConfig& config, size_t node_index,
                                     const std::string& storage_image)
    : impl_(std::make_unique<Impl>(repeater_id, bind_address, port, medium, clock,
                                   config, node_index, storage_image)) {}
CompanionEndpoint::~CompanionEndpoint() { stop(); }

void CompanionEndpoint::start() {
  if (impl_->started) return;
  impl_->serial.start();
  FirmwareContextGuard guard(impl_->context);
  impl_->enterSensorContext();
  impl_->store.begin();
  impl_->mesh.begin(false);
  auto* preferences = impl_->mesh.getNodePrefs();
  preferences->path_hash_mode = 2;
  preferences->freq = impl_->startup_config.frequency_mhz;
  preferences->bw = impl_->startup_config.bandwidth_khz;
  preferences->sf = impl_->startup_config.spreading_factor;
  preferences->cr = impl_->startup_config.coding_rate_denominator;
  impl_->radio.configure(impl_->startup_config);
  impl_->mesh.startInterface(impl_->serial);
  impl_->leaveSensorContext();
  impl_->started = true;
}
void CompanionEndpoint::stop() {
  if (!impl_ || !impl_->started) return;
  impl_->serial.stop();
  impl_->started = false;
}
void CompanionEndpoint::pump() {
  if (!impl_->started) return;
  FirmwareContextGuard guard(impl_->context);
  impl_->enterSensorContext();
  impl_->mesh.loop();
  impl_->leaveSensorContext();
}
const std::string& CompanionEndpoint::repeaterId() const { return impl_->repeater_id; }
uint16_t CompanionEndpoint::port() const { return impl_->port; }
bool CompanionEndpoint::connected() const { return impl_->serial.isConnected(); }

}  // namespace simulator
