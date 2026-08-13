#define MyMesh RepeaterFirmwareMyMesh
#define NodePrefs RepeaterFirmwareNodePrefs
#define board repeater_firmware_board
#define radio_driver repeater_firmware_radio_driver
#define rtc_clock repeater_firmware_rtc_clock
#define sensors repeater_firmware_sensors
#define radio_new_identity repeater_firmware_new_identity
#include "../examples/simple_repeater/MyMesh.h"
#undef MyMesh
#undef NodePrefs
#undef board
#undef radio_driver
#undef rtc_clock
#undef sensors
#undef radio_new_identity

#include "RepeaterFirmware.h"
#include "SimulatorIdentityRegistry.h"

#include <cstring>
#include <stdexcept>

HostBoard repeater_firmware_board;
HostRadioDriver repeater_firmware_radio_driver;
HostRTCClockProxy repeater_firmware_rtc_clock;
SensorManager repeater_firmware_sensors;

bool radio_init() { return true; }

namespace simulator {

class RepeaterFirmware::Impl {
public:
  std::string id;
  LoRaConfig startup_config;
  SimpleMeshTables tables;
  SimRng rng;
  SimulatedRadio radio;
  fs::SPIFFSFS filesystem;
  FirmwareContext context;
  RepeaterFirmwareMyMesh mesh;

  Impl(const DatabaseNode& node, size_t index, SimulatedMedium& medium,
       SimClock& clock, const LoRaConfig& config, uint32_t seed)
      : id(node.id), startup_config(config), rng(seed), radio(medium, clock, config, index),
        context{&radio, &clock, &rng, &filesystem, false},
        mesh(repeater_firmware_board, radio, clock, rng, clock, tables) {
    mesh.self_id = mesh::LocalIdentity(&rng);
    uint8_t prefix[3]{};
    if (id.size() != 6 || !mesh::Utils::fromHex(prefix, sizeof(prefix), id.c_str()))
      throw std::runtime_error("invalid repeater prefix: " + id);
    std::memcpy(mesh.self_id.pub_key, prefix, sizeof(prefix));
    registerForcedRepeaterIdentity(mesh.self_id.pub_key);
  }
};

RepeaterFirmware::RepeaterFirmware(const DatabaseNode& node, size_t index,
                                   SimulatedMedium& medium, SimClock& clock,
                                   const LoRaConfig& config, uint32_t seed)
    : impl_(std::make_unique<Impl>(node, index, medium, clock, config, seed)) {}
RepeaterFirmware::~RepeaterFirmware() = default;

void RepeaterFirmware::begin() {
  FirmwareContextGuard guard(impl_->context);
  impl_->mesh.begin(&impl_->filesystem);
  auto* preferences = impl_->mesh.getNodePrefs();
  preferences->advert_interval = 0;
  preferences->flood_advert_interval = 0;
  preferences->disable_fwd = 0;
  preferences->path_hash_mode = 2;
  preferences->loop_detect = LOOP_DETECT_STRICT;
  preferences->freq = impl_->startup_config.frequency_mhz;
  preferences->bw = impl_->startup_config.bandwidth_khz;
  preferences->sf = impl_->startup_config.spreading_factor;
  preferences->cr = impl_->startup_config.coding_rate_denominator;
  impl_->mesh.updateAdvertTimer();
  impl_->mesh.updateFloodAdvertTimer();
  impl_->radio.configure(impl_->startup_config);
}
void RepeaterFirmware::loop() {
  FirmwareContextGuard guard(impl_->context);
  impl_->mesh.loop();
}
void RepeaterFirmware::injectAdvert() {
  FirmwareContextGuard guard(impl_->context);
  impl_->mesh.sendSelfAdvertisement(0, true);
}
const std::string& RepeaterFirmware::id() const { return impl_->id; }
uint64_t RepeaterFirmware::sentFlood() const { return impl_->mesh.getNumSentFlood(); }
uint64_t RepeaterFirmware::sentDirect() const { return impl_->mesh.getNumSentDirect(); }
uint64_t RepeaterFirmware::receivedFlood() const { return impl_->mesh.getNumRecvFlood(); }
uint64_t RepeaterFirmware::receivedDirect() const { return impl_->mesh.getNumRecvDirect(); }
bool RepeaterFirmware::identityMatchesDatabasePrefix() const {
  uint8_t prefix[3]{};
  return mesh::Utils::fromHex(prefix, sizeof(prefix), impl_->id.c_str()) &&
         std::memcmp(prefix, impl_->mesh.self_id.pub_key, sizeof(prefix)) == 0;
}
const LoRaConfig& RepeaterFirmware::radioConfig() const { return impl_->radio.config(); }

}  // namespace simulator
