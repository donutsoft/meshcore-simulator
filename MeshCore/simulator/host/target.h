#pragma once

#include "../HostPlatform.h"

extern HostBoard board;
extern HostRadioDriver radio_driver;
extern HostRTCClockProxy rtc_clock;
extern SensorManager sensors;

bool radio_init();
mesh::LocalIdentity radio_new_identity();

