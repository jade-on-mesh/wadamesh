// SPDX-License-Identifier: GPL-3.0-or-later
// Team (MeshCore Open TAK team awareness) on this radio: the glue to the mesh,
// GPS and storage, the map overlay, the alarm, Bloodhound, and the Team page.
// Included by UITask.cpp; the rules live in TeamCore.cpp, the bytes in
// TeamWire.h.

#include <new>

#include "TeamCore.h"

#if defined(HAS_GPS_MOTION)
extern bool wadaGpsMotion(float* speed_kmh, float* course_deg);   // board target.cpp
#endif

namespace teamui {

// ---- glue ---------------------------------------------------------------------------
class Io : public team::Io {
 public:
  uint32_t nowMs() override { return millis(); }
  uint32_t epoch() override {
    const uint32_t t = the_mesh.getRTCClock()->getCurrentTime();
    return t > 1700000000UL ? t : 0;
  }
  uint32_t random32() override { return esp_random(); }
  void selfKey4(uint8_t out[4]) override { memcpy(out, the_mesh.getSelfPubKey(), 4); }
  const char* nodeName() override { return the_mesh.getNodePrefs()->node_name; }
  bool fix(team::Fix& f) override {
    UITask* t = g_lv.task;
    if (!t) return false;
    const double la = t->getNodeLat(), lo = t->getNodeLon();
    if (la == 0.0 && lo == 0.0) return false;   // no GPS and no position set by hand
    f = team::Fix();
    f.lat = la;
    f.lon = lo;
    f.gps = t->getGPSState() && t->getGpsFix();
    if (f.gps) {
      f.has_alt = true;
      f.alt_m = t->getGpsAltitude();
      f.accuracy_m = 10;
#if defined(HAS_GPS_MOTION)
      float spd = NAN, crs = NAN;
      if (wadaGpsMotion(&spd, &crs) && !isnan(spd) && !isnan(crs)) {
        f.has_motion = true;
        f.speed_mps = spd / 3.6f;
        f.heading_deg = crs;
      }
#endif
    }
    return true;
  }
  int batteryPct() override {
    const uint16_t mv = batteryMvSmoothed();
    return mv ? batteryPercentFromMv(mv) : -1;
  }
  bool channelOk(const char* name) override { return channelIdx(name) >= 0; }
  bool send(const char* name, const uint8_t* d, size_t n) override {
    const int idx = channelIdx(name);
    return idx >= 0 && the_mesh.voxSendChannelData(idx, TeamWire::kDataType, d, n);
  }
  bool readFile(const char* name, uint8_t* buf, size_t cap, size_t* len) override {
    fs::FS* fs = backupInternalFs();
    char path[24];
    snprintf(path, sizeof path, "/%s", name);
    if (!fs || !fs->exists(path)) return false;
    File f = fs->open(path, "r");
    if (!f) return false;
    const size_t n = f.size();
    if (n > cap) { f.close(); return false; }
    *len = f.read(buf, n);
    f.close();
    return *len == n;
  }
  bool writeFile(const char* name, const uint8_t* d, size_t n) override {
    fs::FS* fs = backupInternalFs();
    if (!fs) return false;
    char path[24];
    snprintf(path, sizeof path, "/%s", name);
    WdtHeavyGuard guard;
    File f = fs->open(path, "w");
    if (!f) return false;
    const size_t w = f.write(d, n);
    f.close();
    return w == n;
  }
  void changed() override { s_version++; }
  void alarm(const team::AlertRec&) override { s_alarm_pending = true; }

  static int channelIdx(const char* name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails cd;
      if (the_mesh.getChannel(i, cd) && cd.name[0] && !strcmp(cd.name, name)) return i;
    }
    return -1;
  }
  static uint32_t s_version;
  static bool s_alarm_pending;
};
uint32_t Io::s_version = 0;
bool Io::s_alarm_pending = false;

static Io s_io;
static team::Core* s_core = nullptr;

static team::Core* core() {
  if (!s_core) {
    void* m = heap_caps_malloc(sizeof(team::Core), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) return nullptr;
    s_core = new (m) team::Core(&s_io);
    s_core->begin();
  }
  return s_core;
}

static void toast(const char* m, int ms = 1800) { if (g_lv.task) g_lv.task->showAlert(m, ms); }

static bool myFix(double* lat, double* lon) {
  team::Fix f;
  if (!s_io.fix(f)) return false;
  *lat = f.lat;
  *lon = f.lon;
  return true;
}

static void rangeTo(double lat, double lon, char* out, size_t cap) {
  double ml, mo;
  if (!myFix(&ml, &mo)) { snprintf(out, cap, "%.5f, %.5f", lat, lon); return; }
  TeamWire::rangeBearing(TeamWire::distanceM(ml, mo, lat, lon), TeamWire::bearingDeg(ml, mo, lat, lon), out, cap);
}

static void showOnMap(double lat, double lon) {
  s_map_center_lat = lat;
  s_map_center_lon = lon;
  s_map_view_inited = true;
  goToTab(MAP_TAB_INDEX);
}

// ---- alarm (a teammate's SOS) ------------------------------------------------------------
static lv_obj_t* s_alarm = nullptr;
static team::AlertRec* s_alarm_rec = nullptr;
static uint32_t s_alarm_beep_ms = 0;

static void alarmClose() {
  if (s_alarm) popupClose(&s_alarm);
  s_alarm_rec = nullptr;
}

static void alarmShow(team::AlertRec& r) {
  alarmClose();
  s_alarm_rec = &r;
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr), sh = lv_disp_get_ver_res(nullptr);
  s_alarm = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_alarm);
  lv_obj_set_size(s_alarm, sw, sh);
  lv_obj_set_style_bg_color(s_alarm, lv_color_hex(0x7A0D12), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_alarm, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(s_alarm, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_alarm, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(s_alarm, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_alarm, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(s_alarm, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_alarm, 8, LV_PART_MAIN);
  char line[160];
  snprintf(line, sizeof line, LV_SYMBOL_WARNING " %s", TeamWire::alertName(r.a.type));
  lv_obj_t* t = lv_label_create(s_alarm);
  lv_label_set_text(t, line);
  lv_obj_set_style_text_font(t, &g_font_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_white(), LV_PART_MAIN);
  char who[64], where[48];
  snprintf(who, sizeof who, "%s", r.a.callsign[0] ? r.a.callsign : "Teammate");
  if (r.a.has_pos) rangeTo(r.a.lat, r.a.lon, where, sizeof where);
  else snprintf(where, sizeof where, "%s", TR("No position"));
  snprintf(line, sizeof line, "%s\n%s%s%s", who, where, r.a.note[0] ? "\n" : "", r.a.note);
  lv_obj_t* d = lv_label_create(s_alarm);
  lv_label_set_text(d, line);
  lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_font(d, &g_font_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(d, lv_color_white(), LV_PART_MAIN);
  useChainedFont(d);
  lv_obj_t* row = lv_obj_create(s_alarm);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
  auto mk = [&](const char* txt, lv_event_cb_t cb) {
    lv_obj_t* b = lv_btn_create(row);
    styleButton(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(b, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, txt);
    useChainedFont(l);
    lv_obj_center(l);
  };
  mk(TR("Acknowledge"), [](lv_event_t*) {
    if (s_alarm_rec && s_core) s_core->acknowledge(*s_alarm_rec);
    alarmClose();
  });
  if (r.a.has_pos) {
    mk(TR("Hound"), [](lv_event_t*) {
      if (s_alarm_rec && s_core) {
        s_core->houndPoint(s_alarm_rec->a.lat, s_alarm_rec->a.lon, s_alarm_rec->a.callsign);
        s_core->acknowledge(*s_alarm_rec);
      }
      alarmClose();
    });
    mk(TR("Map"), [](lv_event_t*) {
      if (!s_alarm_rec || !s_core) return;
      const double la = s_alarm_rec->a.lat, lo = s_alarm_rec->a.lon;
      s_core->acknowledge(*s_alarm_rec);
      alarmClose();
      showOnMap(la, lo);
    });
  }
  lv_obj_move_foreground(s_alarm);
  s_alarm_beep_ms = 0;
}

// ---- Bloodhound strip ----------------------------------------------------------------------
static lv_obj_t* s_hound = nullptr;
static lv_obj_t* s_hound_lbl = nullptr;
static lv_obj_t* s_hound_arrow = nullptr;
static lv_point_t s_hound_pts[5];

static void houndRefresh() {
  team::Core* c = s_core;
  double la, lo, ml, mo;
  char label[33];
  const bool on = c && c->hound(&la, &lo, label, sizeof label);
  if (!on) {
    if (s_hound) popupClose(&s_hound);
    s_hound_lbl = s_hound_arrow = nullptr;
    return;
  }
  if (!s_hound) {
    s_hound = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_hound);
    lv_obj_set_size(s_hound, 168, 40);
    lv_obj_align(s_hound, LV_ALIGN_BOTTOM_RIGHT, -4, -46);
    styleSurface(s_hound, 0x101418, 8);
    lv_obj_set_style_bg_opa(s_hound, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_hound, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_hound, lv_color_hex(0xF2A33A), LV_PART_MAIN);
    lv_obj_add_flag(s_hound, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_hound, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_hound, [](lv_event_t*) {
      showConfirm(TR("Stop Bloodhound?"), TR("Stop"), +[]() { if (s_core) s_core->stopHound(); houndRefresh(); });
    }, LV_EVENT_CLICKED, nullptr);
    s_hound_arrow = lv_line_create(s_hound);
    lv_obj_set_style_line_width(s_hound_arrow, 3, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_hound_arrow, lv_color_hex(0xF2A33A), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(s_hound_arrow, true, LV_PART_MAIN);
    lv_obj_set_pos(s_hound_arrow, 0, 0);
    s_hound_lbl = lv_label_create(s_hound);
    lv_obj_set_style_text_font(s_hound_lbl, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hound_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_pos(s_hound_lbl, 40, 3);
    lv_obj_set_width(s_hound_lbl, 124);
    lv_label_set_long_mode(s_hound_lbl, LV_LABEL_LONG_DOT);
  }
  char txt[96];
  double rel = 0;
  if (myFix(&ml, &mo)) {
    char rb[40];
    const double brg = TeamWire::bearingDeg(ml, mo, la, lo);
    TeamWire::rangeBearing(TeamWire::distanceM(ml, mo, la, lo), brg, rb, sizeof rb);
    snprintf(txt, sizeof txt, "%s\n%s", label, rb);
    rel = brg;   // relative to north: only the M9 has a compass
#if defined(HAS_GPS_MOTION)
    float spd = NAN, crs = NAN;
    if (wadaGpsMotion(&spd, &crs) && !isnan(crs) && spd > 2.0f) rel = brg - crs;   // to travel direction
#endif
  } else {
    snprintf(txt, sizeof txt, "%s\n%s", label, TR("No own position"));
  }
  lv_label_set_text(s_hound_lbl, txt);
  // Arrow: shaft and head around (20, 20), rotated to `rel`.
  const double a = rel * M_PI / 180.0, s = sin(a), co = cos(a);
  auto pt = [&](double x, double y, int i) {   // (x right, y up) before rotation
    s_hound_pts[i].x = (lv_coord_t)lround(20 + x * co + y * s);
    s_hound_pts[i].y = (lv_coord_t)lround(20 + x * s - y * co);
  };
  pt(0, -13, 0); pt(0, 13, 1); pt(-7, 5, 2); pt(0, 13, 3); pt(7, 5, 4);
  lv_line_set_points(s_hound_arrow, s_hound_pts, 5);
  lv_obj_move_foreground(s_hound);
}

// ---- map overlay ---------------------------------------------------------------------------
constexpr int kMapObjs = 112;
static lv_obj_t* s_map_objs[kMapObjs];
static int s_map_obj_n = 0;
struct Hit { int16_t x, y; uint8_t kind; uint8_t idx; };   // kind 0 member, 1 pin
static Hit s_hits[kMapObjs];
static int s_hit_n = 0;

static void mapTrack(lv_obj_t* o) { if (s_map_obj_n < kMapObjs) s_map_objs[s_map_obj_n++] = o; else lv_obj_del(o); }

static void mapFree() {
  for (int i = 0; i < s_map_obj_n; i++) if (s_map_objs[i]) lv_obj_del(s_map_objs[i]);
  s_map_obj_n = 0;
  s_hit_n = 0;
}

static lv_obj_t* mapDot(lv_obj_t* parent, int sx, int sy, int size, uint32_t fill, uint32_t border, int radius, lv_opa_t opa) {
  lv_obj_t* d = lv_obj_create(parent);
  lv_obj_remove_style_all(d);
  lv_obj_set_size(d, size, size);
  lv_obj_set_pos(d, sx - size / 2, sy - size / 2);
  lv_obj_set_style_bg_color(d, lv_color_hex(fill), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(d, opa, LV_PART_MAIN);
  lv_obj_set_style_border_color(d, lv_color_hex(border), LV_PART_MAIN);
  lv_obj_set_style_border_width(d, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(d, radius, LV_PART_MAIN);
  lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  mapTrack(d);
  return d;
}

static void mapLabel(lv_obj_t* parent, int sx, int sy, const char* text, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &g_font_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_color(l, lv_color_hex(0x101418), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(l, LV_OPA_60, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(l, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(l, 3, LV_PART_MAIN);
  lv_obj_set_pos(l, sx + 9, sy - 8);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  mapTrack(l);
}

static bool toScreen(double lat, double lon, double cwx, double cwy, int* sx, int* sy) {
  double wx, wy;
  latLonToWorldPx(lat, lon, s_map_zoom, &wx, &wy);
  *sx = (int)(wx - cwx + k_map_canvas_w / 2);
  *sy = (int)(wy - cwy + k_map_canvas_h / 2);
  return *sx >= -12 && *sx < k_map_canvas_w + 12 && *sy >= -12 && *sy < k_map_canvas_h + 12;
}

}  // namespace teamui

// Called from renderMapMarkers: pins under people, people on top, alerts ringed.
static void teamDrawMapOverlay(lv_obj_t* parent, double cwx, double cwy) {
  using namespace teamui;
  mapFree();
  team::Core* c = s_core;
  if (!c || !c->cfg.enabled) return;
  int sx, sy;
  for (int i = 0; i < team::kMaxMarkers; i++) {
    const TeamWire::Marker* m = c->marker(i);
    if (!m || !toScreen(m->lat, m->lon, cwx, cwy, &sx, &sy)) continue;
    lv_obj_t* d = mapDot(parent, sx, sy, 12, TeamWire::kindRgb(m->kind), 0x101418, 2, LV_OPA_COVER);
    lv_obj_set_style_transform_angle(d, 450, LV_PART_MAIN);   // a diamond, not a contact dot
    lv_obj_set_style_transform_pivot_x(d, 6, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(d, 6, LV_PART_MAIN);
    if (m->label[0] && s_map_zoom >= 13) mapLabel(parent, sx, sy, m->label, 0xFFFFFF);
    if (s_hit_n < kMapObjs) s_hits[s_hit_n++] = {(int16_t)sx, (int16_t)sy, 1, (uint8_t)i};
  }
  for (int i = 0; i < team::kMaxMembers; i++) {
    team::Member* m = c->member(i);
    if (!m || !toScreen(m->pos.lat, m->pos.lon, cwx, cwy, &sx, &sy)) continue;
    const team::Fresh fr = c->freshness(*m);
    if (fr == team::Fresh::Lost) continue;
    if (m->pos.sos) mapDot(parent, sx, sy, 26, 0xE53935, 0xE53935, LV_RADIUS_CIRCLE, LV_OPA_40);
    lv_obj_t* d = mapDot(parent, sx, sy, 16, TeamWire::colorRgb(m->pos.color), 0x101418, LV_RADIUS_CIRCLE,
                         fr == team::Fresh::Live ? LV_OPA_COVER : LV_OPA_50);
    char letter[2] = {TeamWire::roleLetter(m->pos.role), 0};
    lv_obj_t* l = lv_label_create(d);
    lv_label_set_text(l, letter);
    lv_obj_set_style_text_font(l, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_center(l);
    mapLabel(parent, sx, sy, m->pos.callsign[0] ? m->pos.callsign : "?", fr == team::Fresh::Live ? 0xFFFFFF : 0x9AA0A6);
    if (s_hit_n < kMapObjs) s_hits[s_hit_n++] = {(int16_t)sx, (int16_t)sy, 0, (uint8_t)i};
  }
}
static void teamFreeMapOverlay() { teamui::mapFree(); }

namespace teamui {
// ---- member / pin actions -------------------------------------------------------------------
static int s_sel_kind = 0, s_sel_idx = 0;   // what an action sheet is about
static lv_obj_t* s_sheet = nullptr;
static void sheetClose() { if (s_sheet) popupClose(&s_sheet); }

static lv_obj_t* sheetOpen(const char* title, const char* body) {
  sheetClose();
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr), sh = lv_disp_get_ver_res(nullptr);
  s_sheet = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_sheet);
  lv_obj_set_size(s_sheet, sw, sh - STATUSBAR_H);
  lv_obj_set_pos(s_sheet, 0, STATUSBAR_H);
  lv_obj_set_style_bg_color(s_sheet, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_sheet, LV_OPA_50, LV_PART_MAIN);
  lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_sheet, [](lv_event_t* e) {
    if (lv_event_get_target(e) == s_sheet) sheetClose();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* card = lv_obj_create(s_sheet);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, sw - 16, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(card, sh - STATUSBAR_H - 12, LV_PART_MAIN);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -6);
  styleCard(card);
  lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_column(card, 6, LV_PART_MAIN);
  lv_obj_t* t = lv_label_create(card);
  lv_label_set_text(t, title);
  lv_obj_set_width(t, LV_PCT(100));
  lv_obj_set_style_text_font(t, &g_font_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  useChainedFont(t);
  if (body && *body) {
    lv_obj_t* b = lv_label_create(card);
    lv_label_set_text(b, body);
    lv_obj_set_width(b, LV_PCT(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(b, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(b, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
    useChainedFont(b);
  }
  return card;
}

static lv_obj_t* sheetBtn(lv_obj_t* card, const char* txt, lv_event_cb_t cb, void* ud = nullptr, uint32_t bg = 0) {
  lv_obj_t* b = lv_btn_create(card);
  styleButton(b);
  lv_obj_set_size(b, LV_SIZE_CONTENT, 32);
  lv_obj_set_style_pad_hor(b, 10, LV_PART_MAIN);
  if (bg) lv_obj_set_style_bg_color(b, lv_color_hex(bg), LV_PART_MAIN);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  useChainedFont(l);
  lv_obj_center(l);
  return b;
}

static void memberSheet(int i) {
  team::Member* m = s_core ? s_core->member(i) : nullptr;
  if (!m) return;
  s_sel_kind = 0;
  s_sel_idx = i;
  char body[200], rb[40];
  rangeTo(m->pos.lat, m->pos.lon, rb, sizeof rb);
  const uint32_t ago = (millis() - m->heard_ms) / 60000;
  char batt[12] = "";
  if (m->pos.battery_pct >= 0) snprintf(batt, sizeof batt, " · %d%%", m->pos.battery_pct);
  snprintf(body, sizeof body, "%s%s%s%s\n%s · heard %lu min ago%s", TeamWire::roleName(m->pos.role),
           m->pos.status ? " · " : "", TeamWire::statusName(m->pos.status), m->pos.sos ? " · SOS" : "", rb,
           (unsigned long)ago, batt);
  lv_obj_t* card = sheetOpen(m->pos.callsign[0] ? m->pos.callsign : "?", body);
  sheetBtn(card, TR("Hound"), [](lv_event_t*) {
    team::Member* x = s_core ? s_core->member(s_sel_idx) : nullptr;
    if (x) s_core->houndMember(x->pos.key4);
    sheetClose();
    houndRefresh();
  }, nullptr, 0xF2A33A);
  sheetBtn(card, TR("Map"), [](lv_event_t*) {
    team::Member* x = s_core ? s_core->member(s_sel_idx) : nullptr;
    sheetClose();
    if (x) showOnMap(x->pos.lat, x->pos.lon);
  });
  sheetBtn(card, TR("Close"), [](lv_event_t*) { sheetClose(); });
}

static void pinSheet(int i) {
  TeamWire::Marker* m = s_core ? s_core->marker(i) : nullptr;
  if (!m) return;
  s_sel_kind = 1;
  s_sel_idx = i;
  char body[200], rb[40];
  rangeTo(m->lat, m->lon, rb, sizeof rb);
  snprintf(body, sizeof body, "%s%s%s\n%s%s", TeamWire::kindName(m->kind), m->note[0] ? " · " : "", m->note, rb,
           m->att_mode ? "\nHas a photo or voice note: open it in MeshCore Open TAK" : "");
  lv_obj_t* card = sheetOpen(m->label[0] ? m->label : TeamWire::kindName(m->kind), body);
  sheetBtn(card, TR("Hound"), [](lv_event_t*) {
    TeamWire::Marker* x = s_core ? s_core->marker(s_sel_idx) : nullptr;
    if (x) s_core->houndPoint(x->lat, x->lon, x->label[0] ? x->label : TeamWire::kindName(x->kind));
    sheetClose();
    houndRefresh();
  }, nullptr, 0xF2A33A);
  sheetBtn(card, TR("Map"), [](lv_event_t*) {
    TeamWire::Marker* x = s_core ? s_core->marker(s_sel_idx) : nullptr;
    sheetClose();
    if (x) showOnMap(x->lat, x->lon);
  });
  if (s_core->isMine(*m)) {
    sheetBtn(card, TR("Delete"), [](lv_event_t*) {
      TeamWire::Marker* x = s_core ? s_core->marker(s_sel_idx) : nullptr;
      if (x) s_core->deleteMarker(*x);
      sheetClose();
      renderMapMarkers();
    }, nullptr, COLOR_STATUS_DANGER);
  }
  sheetBtn(card, TR("Close"), [](lv_event_t*) { sheetClose(); });
}

// Drop a pin: pick a kind, then an optional label.
static double s_drop_lat = 0, s_drop_lon = 0;
static int s_drop_kind = 0;
static void dropLabelDone(const char* text) {
  if (!text || !s_core) return;
  s_core->putMarker(nullptr, (uint8_t)s_drop_kind, s_drop_lat, s_drop_lon, text, "");
  toast(TR("Pin shared with the team"));
  renderMapMarkers();
}
static void dropSheet(double lat, double lon) {
  s_drop_lat = lat;
  s_drop_lon = lon;
  char body[48];
  snprintf(body, sizeof body, "%.5f, %.5f", lat, lon);
  lv_obj_t* card = sheetOpen(TR("Drop a team pin"), body);
  for (int k = 0; k < TeamWire::kMarkerKinds; k++) {
    lv_obj_t* b = sheetBtn(card, TeamWire::kindName(k), [](lv_event_t* e) {
      s_drop_kind = (int)(intptr_t)lv_event_get_user_data(e);
      sheetClose();
      fmTextPrompt(TR("Label (optional)"), "", dropLabelDone);
    }, (void*)(intptr_t)k);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_hex(TeamWire::kindRgb(k)), LV_PART_MAIN);
  }
}

}  // namespace teamui

// Map canvas tap: true when it hit a teammate or a pin (and handled it).
static bool teamMapTap(lv_point_t p) {
  using namespace teamui;
  if (!s_core || !s_core->cfg.enabled || !s_map_canvas) return false;
  lv_area_t a;
  lv_obj_get_coords(s_map_pan_layer ? s_map_pan_layer : s_map_canvas, &a);
  const int lx = p.x - a.x1, ly = p.y - a.y1;
  int best = -1, best_d = 18 * 18;
  for (int i = 0; i < s_hit_n; i++) {
    const int dx = s_hits[i].x - lx, dy = s_hits[i].y - ly, d = dx * dx + dy * dy;
    if (d <= best_d && (best < 0 || s_hits[i].kind == 0 || s_hits[best].kind == 1)) { best = i; best_d = d; }
  }
  if (best < 0) return false;
  if (s_hits[best].kind == 0) memberSheet(s_hits[best].idx);
  else pinSheet(s_hits[best].idx);
  return true;
}

// Map canvas long-press: drop a pin there.
static bool teamMapLongPress(lv_point_t p) {
  using namespace teamui;
  if (!s_core || !s_core->cfg.enabled || !s_map_canvas) return false;
  if (!s_core->active()) { toast(TR("Team: pick the team channel in Team > Setup")); return true; }
  lv_area_t a;
  lv_obj_get_coords(s_map_canvas, &a);
  double cwx, cwy, lat, lon;
  latLonToWorldPx(s_map_center_lat, s_map_center_lon, s_map_zoom, &cwx, &cwy);
  worldPxToLatLon(cwx + (p.x - a.x1) - k_map_canvas_w / 2, cwy + (p.y - a.y1) - k_map_canvas_h / 2, s_map_zoom, &lat, &lon);
  dropSheet(lat, lon);
  return true;
}

void teamOnChannelData(int idx, const uint8_t* data, size_t len) {
  team::Core* c = teamui::core();
  ChannelDetails cd;
  if (!c || idx < 0 || !the_mesh.getChannel(idx, cd)) return;
  c->onPayload(cd.name, data, len);
}

namespace teamui {
// ---- Team page ----------------------------------------------------------------------------------
static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_body = nullptr;
static int s_tab = 0;   // 0 team, 1 pins, 2 setup
static lv_timer_t* s_page_timer = nullptr;

static void pageBuild();

static lv_obj_t* lbl(lv_obj_t* p, const char* t, const lv_font_t* f, uint32_t col) {
  lv_obj_t* l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(col), LV_PART_MAIN);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  useChainedFont(l);
  return l;
}

static lv_obj_t* rowCard(lv_obj_t* p, lv_event_cb_t cb, void* ud) {
  lv_obj_t* c = lv_obj_create(p);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
  styleCard(c);
  lv_obj_set_style_pad_all(c, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(c, 1, LV_PART_MAIN);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
  return c;
}

static void sosHoldDone() {
  if (!s_core) return;
  if (s_core->sosActive()) s_core->cancelAlert();
  else s_core->raiseAlert(0, "");
  pageBuild();
}

static void buildTeam(lv_obj_t* b) {
  team::Core* c = s_core;
  char line[160];
  // status line
  const int ch = Io::channelIdx(c->cfg.channel);
  if (!c->cfg.enabled) snprintf(line, sizeof line, "%s", TR("Team mode is off. Turn it on in Setup."));
  else if (ch < 0) snprintf(line, sizeof line, "%s", TR("Pick the team channel in Setup."));
  else {
    double la, lo;
    const bool fix = myFix(&la, &lo);
    const uint32_t ago = c->everBeaconed() ? (millis() - c->lastBeaconMs()) / 60000 : 0;
    snprintf(line, sizeof line, "#%s · %s · %s", c->cfg.channel, c->callsign(),
             !c->cfg.share ? "not sharing position" : !fix ? "no position (no GPS fix or set one in Settings)"
             : c->everBeaconed() ? (ago ? "position sent" : "position sent just now") : "sending position");
    (void)ago;
  }
  lbl(b, line, &g_font_12, COLOR_SUB);
  // SOS
  lv_obj_t* sos = lv_btn_create(b);
  lv_obj_set_size(sos, LV_PCT(100), 40);
  styleButton(sos);
  lv_obj_set_style_bg_color(sos, lv_color_hex(c->sosActive() ? 0x5A5F66 : 0xD32F2F), LV_PART_MAIN);
  lv_obj_add_event_cb(sos, [](lv_event_t*) {
    if (!s_core || !s_core->active()) { toast(TR("Team is not set up yet")); return; }
    if (s_core->sosActive())
      showConfirm(TR("Cancel your SOS?"), TR("Cancel SOS"), sosHoldDone);
    else
      showConfirm(TR("Send SOS to the team with your position?"), TR("Send SOS"), sosHoldDone);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* sl = lv_label_create(sos);
  lv_label_set_text(sl, c->sosActive() ? TR("SOS active: tap to cancel") : LV_SYMBOL_WARNING " SOS");
  lv_obj_set_style_text_font(sl, &g_font_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(sl, lv_color_white(), LV_PART_MAIN);
  useChainedFont(sl);
  lv_obj_center(sl);
  // active alerts first
  for (int i = 0; i < team::kMaxAlerts; i++) {
    team::AlertRec* r = c->alert(i);
    if (!r || !r->a.active) continue;
    char rb[40] = "";
    if (r->a.has_pos) rangeTo(r->a.lat, r->a.lon, rb, sizeof rb);
    snprintf(line, sizeof line, LV_SYMBOL_WARNING " %s: %s  %s", TeamWire::alertName(r->a.type),
             r->a.callsign[0] ? r->a.callsign : "?", rb);
    lv_obj_t* card = rowCard(b, [](lv_event_t* e) {
      team::AlertRec* x = s_core ? s_core->alert((int)(intptr_t)lv_event_get_user_data(e)) : nullptr;
      if (x) alarmShow(*x);
    }, (void*)(intptr_t)i);
    lv_obj_set_style_border_color(card, lv_color_hex(0xE53935), LV_PART_MAIN);
    lbl(card, line, &g_font_14, 0xFF8A80);
  }
  // roster, nearest first
  int order[team::kMaxMembers], n = 0;
  double dist[team::kMaxMembers];
  double ml = 0, mo = 0;
  const bool fix = myFix(&ml, &mo);
  for (int i = 0; i < team::kMaxMembers; i++) {
    team::Member* m = c->member(i);
    if (!m) continue;
    dist[i] = fix ? TeamWire::distanceM(ml, mo, m->pos.lat, m->pos.lon) : 0;
    order[n++] = i;
  }
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && dist[order[j]] < dist[order[j - 1]]; j--) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
  if (!n && c->cfg.enabled) lbl(b, TR("No teammates heard yet."), &g_font_12, COLOR_SUB);
  for (int k = 0; k < n; k++) {
    team::Member* m = c->member(order[k]);
    const team::Fresh fr = c->freshness(*m);
    lv_obj_t* card = rowCard(b, [](lv_event_t* e) { memberSheet((int)(intptr_t)lv_event_get_user_data(e)); },
                             (void*)(intptr_t)order[k]);
    lv_obj_set_style_border_color(card, lv_color_hex(TeamWire::colorRgb(m->pos.color)), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    char rb[40];
    rangeTo(m->pos.lat, m->pos.lon, rb, sizeof rb);
    snprintf(line, sizeof line, "%s%s  %s", m->pos.sos ? LV_SYMBOL_WARNING " " : "", m->pos.callsign[0] ? m->pos.callsign : "?", rb);
    lbl(card, line, &g_font_14, fr == team::Fresh::Live ? COLOR_TEXT : COLOR_SUB);
    const uint32_t ago = (millis() - m->heard_ms) / 60000;
    char batt[12] = "";
    if (m->pos.battery_pct >= 0) snprintf(batt, sizeof batt, " · %d%%", m->pos.battery_pct);
    snprintf(line, sizeof line, "%s%s%s · %lu min%s%s", TeamWire::roleName(m->pos.role), m->pos.status ? " · " : "",
             TeamWire::statusName(m->pos.status), (unsigned long)ago, batt,
             fr == team::Fresh::Live ? "" : fr == team::Fresh::Stale ? " · stale" : " · lost");
    lbl(card, line, &g_font_12, COLOR_SUB);
  }
}

static void buildPins(lv_obj_t* b) {
  team::Core* c = s_core;
  lv_obj_t* add = lv_btn_create(b);
  styleButton(add);
  lv_obj_set_size(add, LV_PCT(100), 32);
  lv_obj_add_event_cb(add, [](lv_event_t*) {
    double la, lo;
    if (!s_core || !s_core->active()) { toast(TR("Team is not set up yet")); return; }
    if (!myFix(&la, &lo)) { toast(TR("No position: long-press the map instead"), 2200); return; }
    dropSheet(la, lo);
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* al = lv_label_create(add);
  lv_label_set_text(al, TR("Pin at my position  (or long-press the map)"));
  useChainedFont(al);
  lv_obj_center(al);
  int n = 0;
  char line[120];
  for (int i = 0; i < team::kMaxMarkers; i++) {
    TeamWire::Marker* m = c->marker(i);
    if (!m) continue;
    n++;
    lv_obj_t* card = rowCard(b, [](lv_event_t* e) { pinSheet((int)(intptr_t)lv_event_get_user_data(e)); }, (void*)(intptr_t)i);
    lv_obj_set_style_border_color(card, lv_color_hex(TeamWire::kindRgb(m->kind)), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    char rb[40];
    rangeTo(m->lat, m->lon, rb, sizeof rb);
    snprintf(line, sizeof line, "%s  %s", m->label[0] ? m->label : TeamWire::kindName(m->kind), rb);
    lbl(card, line, &g_font_14, COLOR_TEXT);
    snprintf(line, sizeof line, "%s%s%s%s", TeamWire::kindName(m->kind), m->note[0] ? " · " : "", m->note,
             c->isMine(*m) ? " · mine" : "");
    lbl(card, line, &g_font_12, COLOR_SUB);
  }
  if (!n) lbl(b, TR("No pins yet."), &g_font_12, COLOR_SUB);
}

static void setupCycle(lv_event_t* e) {
  team::Core* c = s_core;
  switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case 0: c->cfg.enabled = !c->cfg.enabled; break;
    case 1: c->cfg.role = (uint8_t)((c->cfg.role + 1) % TeamWire::kRoles); break;
    case 2: c->cfg.color = (uint8_t)((c->cfg.color + 1) % TeamWire::kColors); break;
    case 3: c->cfg.status = (uint8_t)((c->cfg.status + 1) % TeamWire::kStatuses); break;
    case 4: c->cfg.share = !c->cfg.share; break;
  }
  c->identityChanged();
  renderMapMarkers();
  pageBuild();
}

static void callsignDone(const char* t) {
  if (!t || !s_core) return;
  snprintf(s_core->cfg.callsign, sizeof s_core->cfg.callsign, "%s", t);
  s_core->identityChanged();
  pageBuild();
}

static void buildSetup(lv_obj_t* b) {
  team::Core* c = s_core;
  char line[96];
  auto btn = [&](const char* t, int which, uint32_t bg = 0) {
    lv_obj_t* x = lv_btn_create(b);
    styleButton(x);
    lv_obj_set_size(x, LV_PCT(100), 32);
    if (bg) lv_obj_set_style_bg_color(x, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_add_event_cb(x, setupCycle, LV_EVENT_CLICKED, (void*)(intptr_t)which);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, t);
    useChainedFont(l);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  };
  snprintf(line, sizeof line, "%s: %s", TR("Team mode"), c->cfg.enabled ? TR("on") : TR("off"));
  btn(line, 0, c->cfg.enabled ? 0x2E7D32 : 0);
  lbl(b, TR("Team channel (private channels only):"), &g_font_12, COLOR_SUB);
  lv_obj_t* row = lv_obj_create(b);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(row, 6, LV_PART_MAIN);
  int shown = 0;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(i, cd) || !cd.name[0] || cd.name[0] == '#') continue;
    if (!strcasecmp(cd.name, "public")) continue;   // a pad-free broadcast to strangers
    lv_obj_t* x = lv_btn_create(row);
    styleButton(x);
    lv_obj_set_size(x, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_pad_hor(x, 10, LV_PART_MAIN);
    if (!strcmp(cd.name, c->cfg.channel)) lv_obj_set_style_bg_color(x, lv_color_hex(0x0E7C86), LV_PART_MAIN);
    lv_obj_add_event_cb(x, [](lv_event_t* e) {
      ChannelDetails d;
      if (s_core && the_mesh.getChannel((int)(intptr_t)lv_event_get_user_data(e), d)) {
        snprintf(s_core->cfg.channel, sizeof s_core->cfg.channel, "%s", d.name);
        s_core->identityChanged();
        pageBuild();
      }
    }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, cd.name);
    useChainedFont(l);
    lv_obj_center(l);
    shown++;
  }
  if (!shown) lbl(row, TR("No private channels. Join the team's channel first (scan its QR in MeshCore Open TAK's channel settings, or add it in Chats)."), &g_font_12, 0xF2A33A);
  {
    lv_obj_t* x = lv_btn_create(b);
    styleButton(x);
    lv_obj_set_size(x, LV_PCT(100), 32);
    lv_obj_add_event_cb(x, [](lv_event_t*) {
      fmTextPrompt(TR("Callsign"), s_core ? s_core->cfg.callsign : "", callsignDone);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* l = lv_label_create(x);
    snprintf(line, sizeof line, "%s: %s", TR("Callsign"), c->callsign());
    lv_label_set_text(l, line);
    useChainedFont(l);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  }
  snprintf(line, sizeof line, "%s: %s", TR("Role"), TeamWire::roleName(c->cfg.role));
  btn(line, 1);
  snprintf(line, sizeof line, "%s: %s", TR("Colour"), TeamWire::colorName(c->cfg.color));
  btn(line, 2, TeamWire::colorRgb(c->cfg.color) == 0xFFFFFF ? 0 : TeamWire::colorRgb(c->cfg.color));
  snprintf(line, sizeof line, "%s: %s", TR("Status"), c->cfg.status ? TeamWire::statusName(c->cfg.status) : TR("none"));
  btn(line, 3);
  snprintf(line, sizeof line, "%s: %s", TR("Share my position"), c->cfg.share ? TR("yes") : TR("no"));
  btn(line, 4);
  lbl(b, TR("Your position comes from GPS, or from the position set in Settings when there is no fix. "
            "It is sent often while moving and every 10 minutes when still."), &g_font_12, COLOR_SUB);
}

static void pageClose();
static void pageBack() { pageClose(); }

static void pageBuild() {
  if (!s_root || !s_core) return;
  if (s_body) { lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN); lv_obj_del_async(s_body); }
  s_body = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_body);
  lv_obj_set_size(s_body, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(s_body, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_body, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
  // tabs
  lv_obj_t* tabs = lv_obj_create(s_body);
  lv_obj_remove_style_all(tabs);
  lv_obj_set_size(tabs, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(tabs, 6, LV_PART_MAIN);
  static const char* const names[3] = {"Team", "Pins", "Setup"};
  for (int i = 0; i < 3; i++) {
    lv_obj_t* x = lv_btn_create(tabs);
    styleButton(x);
    lv_obj_set_size(x, LV_SIZE_CONTENT, 28);
    lv_obj_set_flex_grow(x, 1);
    if (i == s_tab) lv_obj_set_style_bg_color(x, lv_color_hex(0x0E7C86), LV_PART_MAIN);
    lv_obj_add_event_cb(x, [](lv_event_t* e) { s_tab = (int)(intptr_t)lv_event_get_user_data(e); pageBuild(); },
                        LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, TR(names[i]));
    useChainedFont(l);
    lv_obj_center(l);
  }
  if (s_tab == 0) buildTeam(s_body);
  else if (s_tab == 1) buildPins(s_body);
  else buildSetup(s_body);
}

static void pageClose() {
  if (s_page_timer) { lv_timer_del(s_page_timer); s_page_timer = nullptr; }
  sheetClose();
  s_body = nullptr;
  if (s_root) { lv_obj_t* r = s_root; s_root = nullptr; appPageDeleteRootAsync(r); }
  appPageEnd(pageBack);
}

static void pageOpen() {
  if (!core()) { toast(TR("Out of memory")); return; }
  if (s_root) return;
  s_root = appPageCreateRoot(COLOR_BG);
  appPageBeginSlim("Team", pageBack);
  s_tab = (s_core->cfg.enabled && Io::channelIdx(s_core->cfg.channel) >= 0) ? 0 : 2;
  pageBuild();
  // A roster refresh at most every 3 s, and only when something changed.
  s_page_timer = lv_timer_create([](lv_timer_t*) {
    static uint32_t seen = 0;
    if (Io::s_version != seen && s_tab != 2) pageBuild();
    seen = Io::s_version;
  }, 3000, nullptr);
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

// ---- loop ---------------------------------------------------------------------------------------
static void loopTick() {
  team::Core* c = core();
  if (!c) return;
  c->tick();
  const uint32_t now = millis();
  static uint32_t map_seen = 0, last_map = 0;
  if (Io::s_version != map_seen && now - last_map > 1500) {
    map_seen = Io::s_version;
    last_map = now;
    if (s_map_canvas) renderMapMarkers();
  }
  // Alarm: show the newest unacknowledged alert; chime every 4 s while it is up.
  if (Io::s_alarm_pending || (!s_alarm && c->firstUnacked())) {
    Io::s_alarm_pending = false;
    if (team::AlertRec* r = c->firstUnacked()) alarmShow(*r);
  }
  if (s_alarm && now - s_alarm_beep_ms >= 4000) {
    s_alarm_beep_ms = now;
    if (g_lv.task && !g_lv.task->isBuzzerQuiet()) uiPlaySlot(TOUCH_SND_MEN);
  }
  static uint32_t last_hound = 0;
  if (now - last_hound >= 1000) { last_hound = now; houndRefresh(); }
}

}  // namespace teamui
