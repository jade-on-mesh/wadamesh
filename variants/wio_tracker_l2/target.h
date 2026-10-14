// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#include <helpers/ui/MomentaryButton.h>
#include "../../src/helpers/ClockFloorRTC.h"
#include "helpers/sensors/EnvironmentSensorManager.h"
#include "helpers/sensors/MicroNMEALocationProvider.h"

#include "WioTrackerL2Board.h"
#include "WioTrackerL2Display.h"

#include "../../src/helpers/MediaMeshRadio.h"   // RADIO_DRIVER_CLASS (MediaMesh radio2 scan)

extern WioTrackerL2Board board;
extern RADIO_CLASS radio;
extern RADIO_DRIVER_CLASS radio_driver;   // MediaMesh-capable wrapper where supported
extern ClockFloorRTC rtc_clock;
extern EnvironmentSensorManager sensors;
extern WioTrackerL2Display display;
extern MomentaryButton user_btn;

bool radio_init();
mesh::LocalIdentity radio_new_identity();