// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The map icons of the MeshCore Open TAK app (Material Icons), in map_icons_14
// (scripts/build/gen-map-icons.sh), so the device map draws the same icon for
// the same thing: team roles (lib/team/team_map.dart teamRoleIcon), team
// marker kinds (markerKindIcon), watched reports, contacts (map_screen.dart
// _getNodeIcon) and shared pins. UTF-8 string literals for an lv_label.

#include <stdint.h>

#include "lvgl.h"

extern "C" const lv_font_t map_icons_14;

namespace mapicons {

constexpr const char* kPerson = "\xEE\x9F\xBD";   // person
constexpr const char* kStar = "\xEE\xA0\xB8";   // star
constexpr const char* kMedical = "\xEF\x84\x89";   // medical_services
constexpr const char* kExplore = "\xEE\xA1\xBA";   // explore
constexpr const char* kCellTower = "\xEE\xAE\xBA";   // cell_tower
constexpr const char* kCar = "\xEE\x94\xB1";   // directions_car
constexpr const char* kShield = "\xEF\xA3\xA0";   // shield_outlined
constexpr const char* kVisibility = "\xEE\xA3\xB4";   // visibility
constexpr const char* kBank = "\xEE\xA1\x8F";   // account_balance
constexpr const char* kVideocam = "\xEF\xA3\xA1";   // videocam_outlined
constexpr const char* kBadge = "\xEF\xA3\xA2";   // badge_outlined
constexpr const char* kPlace = "\xEE\x95\x9F";   // place
constexpr const char* kPersonPin = "\xEE\x95\xAA";   // person_pin_circle
constexpr const char* kFlag = "\xEE\x85\x93";   // flag
constexpr const char* kSearch = "\xEE\xA2\xB6";   // search
constexpr const char* kWarningAmber = "\xEF\x82\x83";   // warning_amber
constexpr const char* kFlightLand = "\xEE\xA4\x84";   // flight_land
constexpr const char* kHospital = "\xEE\x95\x88";   // local_hospital
constexpr const char* kEmojiPeople = "\xEE\xA8\x9D";   // emoji_people
constexpr const char* kWaterDrop = "\xEE\x9E\x98";   // water_drop
constexpr const char* kCheckCircle = "\xEE\xA4\xAD";   // check_circle_outline
constexpr const char* kFlight = "\xEE\x94\xB9";   // flight
constexpr const char* kWaves = "\xEE\x85\xB6";   // waves
constexpr const char* kRouter = "\xEE\x8C\xA8";   // router
constexpr const char* kMeetingRoom = "\xEE\xAD\x8F";   // meeting_room
constexpr const char* kSensors = "\xEE\x94\x9E";   // sensors
constexpr const char* kDeviceUnknown = "\xEE\x8C\xB9";   // device_unknown
constexpr const char* kNavigation = "\xEE\x95\x9D";   // navigation
constexpr const char* kMic = "\xEE\x80\xA9";   // mic
constexpr const char* kSos = "\xEE\xAF\xB7";   // sos
constexpr const char* kWarning = "\xEE\x80\x82";   // warning
constexpr const char* kNotify = "\xEE\x9F\xB7";   // notifications_active

// A team role's icon: the icon the team chose from the app's library when it is
// one of these, else (no choice) the role's own, as the app does. nullptr when
// the chosen library icon isn't on this device: show the role's letter instead.
inline const char* role(int role_id, uint16_t library_icon) {
  struct Lib { uint16_t id; const char* glyph; };
  static const Lib kLib[] = {
      {1, kPerson},
      {2, kStar},
      {3, kMedical},
      {4, kExplore},
      {5, kCellTower},
      {6, kCar},
      {7, kShield},
      {8, kVisibility},
      {9, kBank},
      {24, kEmojiPeople},
      {55, kPersonPin},
      {64, kHospital},
      {87, kWaterDrop},
      {91, kSos},
      {106, kWarning},
      {107, kWarningAmber},
      {118, kSensors},
      {135, kRouter},
      {142, kMic},
      {216, kFlight},
      {218, kFlightLand},
      {241, kNavigation},
      {248, kPlace},
      {258, kFlag},
      {302, kWaves},
      {449, kMeetingRoom},
      {566, kVideocam},
  };
  if (library_icon) {
    for (const Lib& l : kLib) if (l.id == library_icon) return l.glyph;
    return nullptr;
  }
  static const char* const kDefault[10] = {kPerson, kStar,  kMedical,    kExplore, kCellTower,
                                           kCar,    kShield, kVisibility, kBank,    kVideocam};
  return role_id >= 0 && role_id < 10 ? kDefault[role_id] : kBadge;
}

// A team marker kind's icon (TeamWire marker kinds, in order).
inline const char* markerKind(int kind) {
  static const char* const k[10] = {kPlace,    kPersonPin, kFlag,        kSearch,     kWarningAmber,
                                    kFlightLand, kHospital, kEmojiPeople, kWaterDrop, kCheckCircle};
  return kind >= 0 && kind < 10 ? k[kind] : kPlace;
}

// A contact's icon by its advert type (1 chat, 2 repeater, 3 room, 4 sensor).
inline const char* contactType(uint8_t type) {
  switch (type) {
    case 1: return kPerson;
    case 2: return kRouter;
    case 3: return kMeetingRoom;
    case 4: return kSensors;
    default: return kDeviceUnknown;
  }
}

}  // namespace mapicons
