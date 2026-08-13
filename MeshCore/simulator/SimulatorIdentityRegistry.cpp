#include "SimulatorIdentityRegistry.h"

#include <array>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace simulator {
namespace {
struct KeyHash {
  size_t operator()(const std::array<uint8_t, 32>& key) const {
    size_t hash = 1469598103934665603ULL;
    for (const uint8_t byte : key) hash = (hash ^ byte) * 1099511628211ULL;
    return hash;
  }
};
std::unordered_set<std::array<uint8_t, 32>, KeyHash> identities;
std::mutex identities_mutex;
}  // namespace

void registerForcedRepeaterIdentity(const uint8_t public_key[32]) {
  std::array<uint8_t, 32> key{};
  std::memcpy(key.data(), public_key, key.size());
  std::lock_guard<std::mutex> lock(identities_mutex);
  identities.insert(key);
}
bool isForcedRepeaterIdentity(const uint8_t public_key[32]) {
  std::array<uint8_t, 32> key{};
  std::memcpy(key.data(), public_key, key.size());
  std::lock_guard<std::mutex> lock(identities_mutex);
  return identities.count(key) != 0;
}

}  // namespace simulator

