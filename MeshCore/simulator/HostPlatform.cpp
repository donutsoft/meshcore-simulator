#include "HostPlatform.h"

#include <Arduino.h>
#include <CayenneLPP.h>
#include <FS.h>
#include <SPIFFS.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>

namespace {
thread_local simulator::FirmwareContext* current_context = nullptr;
std::mt19937 arduino_random{1};
}

HostSerial Serial;
fs::SPIFFSFS SPIFFS;

size_t Print::printf(const char* format, ...) {
  char buffer[512];
  va_list arguments;
  va_start(arguments, format);
  const int length = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
  va_end(arguments);
  if (length <= 0) return 0;
  return write(reinterpret_cast<const uint8_t*>(buffer),
               std::min(static_cast<size_t>(length), sizeof(buffer) - 1));
}

unsigned long millis() {
  auto* context = simulator::activeFirmwareContext();
  return context && context->clock ? context->clock->getMillis() : 0;
}
void randomSeed(unsigned long seed) { arduino_random.seed(static_cast<uint32_t>(seed)); }
long random(long maximum) { return random(0, maximum); }
long random(long minimum, long maximum) {
  if (maximum <= minimum) return minimum;
  std::uniform_int_distribution<long> distribution(minimum, maximum - 1);
  return distribution(arduino_random);
}
char* ltoa(long value, char* destination, int base) {
  if (base == 16)
    std::sprintf(destination, "%lX", value);
  else
    std::sprintf(destination, "%ld", value);
  return destination;
}

namespace fs {

namespace {
constexpr char kImageMagic[] = "MCSFS01";

template <typename Value>
bool readValue(std::istream& input, Value& value) {
  return static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template <typename Value>
void writeValue(std::ostream& output, const Value& value) {
  output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
}  // namespace

struct File::State {
  FS* owner = nullptr;
  std::string path;
  std::shared_ptr<std::vector<uint8_t>> bytes;
  size_t position = 0;
  bool writable = false;
  bool directory = false;
  std::vector<std::string> children;
  size_t child_index = 0;
  bool open = true;
};

std::string FS::normalise(const char* raw) {
  if (!raw || !*raw) return "/";
  std::string path(raw);
  if (path.front() != '/') path.insert(path.begin(), '/');
  while (path.size() > 1 && path.back() == '/') path.pop_back();
  return path;
}

bool FS::attachImage(const std::string& path) {
  image_path_ = path;
  files_.clear();
  directories_ = {"/"};
  return loadImage();
}

bool FS::loadImage() {
  if (image_path_.empty() || !std::filesystem::exists(image_path_)) return true;
  std::ifstream input(image_path_, std::ios::binary);
  char magic[sizeof(kImageMagic)]{};
  if (!input.read(magic, sizeof(magic)) || std::memcmp(magic, kImageMagic, sizeof(magic)) != 0)
    return false;
  uint32_t directory_count = 0;
  uint32_t file_count = 0;
  if (!readValue(input, directory_count) || !readValue(input, file_count) ||
      directory_count > 4096 || file_count > 4096)
    return false;
  auto readPath = [&](std::string& path) {
    uint32_t length = 0;
    if (!readValue(input, length) || length == 0 || length > 4096) return false;
    path.resize(length);
    return static_cast<bool>(input.read(path.data(), length));
  };
  for (uint32_t index = 0; index < directory_count; ++index) {
    std::string path;
    if (!readPath(path)) return false;
    directories_.insert(normalise(path.c_str()));
  }
  for (uint32_t index = 0; index < file_count; ++index) {
    std::string path;
    uint64_t length = 0;
    if (!readPath(path) || !readValue(input, length) || length > totalBytes()) return false;
    auto bytes = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(length));
    if (length && !input.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(length)))
      return false;
    files_[normalise(path.c_str())] = std::move(bytes);
  }
  return true;
}

bool FS::persistImage() const {
  if (image_path_.empty()) return true;
  const std::filesystem::path destination(image_path_);
  std::error_code error;
  if (!destination.parent_path().empty())
    std::filesystem::create_directories(destination.parent_path(), error);
  if (error) return false;
  const std::filesystem::path temporary = destination.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(kImageMagic, sizeof(kImageMagic));
  const uint32_t directory_count = static_cast<uint32_t>(directories_.size());
  const uint32_t file_count = static_cast<uint32_t>(files_.size());
  writeValue(output, directory_count);
  writeValue(output, file_count);
  auto writePath = [&](const std::string& path) {
    const uint32_t length = static_cast<uint32_t>(path.size());
    writeValue(output, length);
    output.write(path.data(), path.size());
  };
  for (const auto& path : directories_) writePath(path);
  for (const auto& entry : files_) {
    writePath(entry.first);
    const uint64_t length = entry.second->size();
    writeValue(output, length);
    if (length)
      output.write(reinterpret_cast<const char*>(entry.second->data()),
                   static_cast<std::streamsize>(length));
  }
  output.close();
  if (!output) return false;
  std::filesystem::rename(temporary, destination, error);
  if (!error) return true;
  std::filesystem::remove(destination, error);
  error.clear();
  std::filesystem::rename(temporary, destination, error);
  return !error;
}

File FS::open(const char* raw_path, const char* raw_mode, bool create) {
  const std::string path = normalise(raw_path);
  const std::string mode = raw_mode ? raw_mode : "r";
  if (directories_.count(path)) {
    auto state = std::make_shared<File::State>();
    state->owner = this;
    state->path = path;
    state->directory = true;
    const std::string prefix = path == "/" ? "/" : path + "/";
    for (const auto& entry : files_) {
      if (entry.first.rfind(prefix, 0) == 0 && entry.first.find('/', prefix.size()) == std::string::npos)
        state->children.push_back(entry.first);
    }
    return File(state);
  }
  const bool writable = mode.find('w') != std::string::npos || mode.find('a') != std::string::npos;
  auto found = files_.find(path);
  if (found == files_.end()) {
    if (!writable && !create) return File();
    found = files_.emplace(path, std::make_shared<std::vector<uint8_t>>()).first;
  }
  if (mode.find('w') != std::string::npos) found->second->clear();
  auto state = std::make_shared<File::State>();
  state->owner = this;
  state->path = path;
  state->bytes = found->second;
  state->writable = writable;
  state->position = mode.find('a') != std::string::npos ? state->bytes->size() : 0;
  return File(state);
}

bool FS::exists(const char* path) const {
  const std::string value = normalise(path);
  return files_.count(value) || directories_.count(value);
}
bool FS::remove(const char* path) {
  const bool removed = files_.erase(normalise(path)) != 0;
  return removed && persistImage();
}
bool FS::mkdir(const char* path) {
  const bool inserted = directories_.insert(normalise(path)).second;
  return inserted && persistImage();
}
bool FS::format() {
  files_.clear();
  directories_ = {"/"};
  return persistImage();
}
size_t FS::usedBytes() const {
  size_t total = 0;
  for (const auto& entry : files_) total += entry.second->size();
  return total;
}

File::operator bool() const { return state_ && state_->open; }
int File::available() {
  return state_ && state_->bytes && state_->position < state_->bytes->size()
             ? static_cast<int>(state_->bytes->size() - state_->position)
             : 0;
}
int File::read() {
  uint8_t value = 0;
  return read(&value, 1) == 1 ? value : -1;
}
size_t File::read(uint8_t* destination, size_t length) {
  if (!state_ || !state_->open || !state_->bytes) return 0;
  const size_t count = std::min(length, state_->bytes->size() - std::min(state_->position, state_->bytes->size()));
  if (count) std::memcpy(destination, state_->bytes->data() + state_->position, count);
  state_->position += count;
  return count;
}
size_t File::write(uint8_t byte) { return write(&byte, 1); }
size_t File::write(const uint8_t* source, size_t length) {
  if (!state_ || !state_->open || !state_->bytes || !state_->writable) return 0;
  if (state_->position + length > state_->bytes->size()) state_->bytes->resize(state_->position + length);
  std::memcpy(state_->bytes->data() + state_->position, source, length);
  state_->position += length;
  return length;
}
bool File::seek(size_t position) {
  if (!state_ || !state_->open || !state_->bytes) return false;
  state_->position = position;
  return true;
}
size_t File::size() const { return state_ && state_->bytes ? state_->bytes->size() : 0; }
void File::close() {
  if (!state_) return;
  if (state_->open && state_->writable && state_->owner) state_->owner->persistImage();
  state_->open = false;
}
bool File::isDirectory() const { return state_ && state_->directory; }
const char* File::name() const { return state_ ? state_->path.c_str() : ""; }
File File::openNextFile() {
  if (!state_ || !state_->directory || state_->child_index >= state_->children.size()) return File();
  return state_->owner->open(state_->children[state_->child_index++].c_str(), "r");
}

}  // namespace fs

bool CayenneLPP::addVoltage(uint8_t channel, float voltage) {
  if (buffer_.size() + 4 > maximum_) return false;
  const uint16_t scaled = static_cast<uint16_t>(std::max(0.0f, voltage) * 100.0f);
  buffer_.insert(buffer_.end(), {channel, 0x74, static_cast<uint8_t>(scaled >> 8), static_cast<uint8_t>(scaled)});
  return true;
}
bool CayenneLPP::addTemperature(uint8_t channel, float temperature) {
  if (buffer_.size() + 4 > maximum_) return false;
  const int16_t scaled = static_cast<int16_t>(temperature * 10.0f);
  buffer_.insert(buffer_.end(), {channel, 0x67, static_cast<uint8_t>(scaled >> 8), static_cast<uint8_t>(scaled)});
  return true;
}

namespace simulator {
FirmwareContext* activeFirmwareContext() { return current_context; }
FirmwareContextGuard::FirmwareContextGuard(FirmwareContext& context) : previous_(current_context) {
  current_context = &context;
}
FirmwareContextGuard::~FirmwareContextGuard() { current_context = previous_; }
}  // namespace simulator

void HostBoard::reboot() {
  if (auto* context = simulator::activeFirmwareContext()) context->reboot_requested = true;
}
uint32_t HostRTCClockProxy::getCurrentTime() {
  auto* context = simulator::activeFirmwareContext();
  return context && context->clock ? context->clock->getCurrentTime() : 0;
}
void HostRTCClockProxy::setCurrentTime(uint32_t time) {
  auto* context = simulator::activeFirmwareContext();
  if (context && context->clock) context->clock->setCurrentTime(time);
}
void HostRadioDriver::setParams(float frequency, float bandwidth, uint8_t spreading_factor, uint8_t coding_rate) {
  auto* context = simulator::activeFirmwareContext();
  if (context && context->radio) context->radio->setParams(frequency, bandwidth, spreading_factor, coding_rate);
}
float HostRadioDriver::getLastRSSI() const {
  auto* context = simulator::activeFirmwareContext();
  return context && context->radio ? context->radio->getLastRSSI() : 0;
}
float HostRadioDriver::getLastSNR() const {
  auto* context = simulator::activeFirmwareContext();
  return context && context->radio ? context->radio->getLastSNR() : 0;
}
uint32_t HostRadioDriver::getPacketsRecv() const {
  auto* context = simulator::activeFirmwareContext();
  return context && context->radio ? context->radio->packetsReceived() : 0;
}
uint32_t HostRadioDriver::getPacketsSent() const {
  auto* context = simulator::activeFirmwareContext();
  return context && context->radio ? context->radio->packetsSent() : 0;
}
void HostRadioDriver::resetStats() {
  auto* context = simulator::activeFirmwareContext();
  if (context && context->radio) context->radio->resetStats();
}
