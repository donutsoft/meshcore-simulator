#pragma once

#include <cstdint>

namespace simulator {

void registerForcedRepeaterIdentity(const uint8_t public_key[32]);
bool isForcedRepeaterIdentity(const uint8_t public_key[32]);

}  // namespace simulator

