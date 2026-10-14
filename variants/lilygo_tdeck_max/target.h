// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#include <helpers/ui/MomentaryButton.h>

#include "TDeckMaxBoard.h"
#include "TDeckProDisplay.h"   // reused from variants/lilygo_tdeck_pro (second -I in the env)
#include "TDeckMaxGps.h"
#include "../../src/helpers/ClockFloorRTC.h"
#include "helpers/sensors/EnvironmentSensorManager.h"

#include "../../src/helpers/MediaMeshRadio.h"   // RADIO_DRIVER_CLASS (MediaMesh radio2 scan)

extern TDeckMaxBoard board;
extern RADIO_DRIVER_CLASS radio_driver;   // MediaMesh-capable wrapper where supported
extern RADIO_CLASS radio;
extern ClockFloorRTC rtc_clock;
extern EnvironmentSensorManager sensors;
extern TDeckProDisplay display;
extern MomentaryButton user_btn;

SPIClass* tdeckSharedSPI();
bool radio_init();
mesh::LocalIdentity radio_new_identity();