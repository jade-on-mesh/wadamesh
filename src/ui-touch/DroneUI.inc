// SPDX-License-Identifier: GPL-3.0-or-later
// Drone Detector on this standalone: Remote ID (ASTM F3411 / Open Drone ID)
// from Wi-Fi beacons and BLE adverts, the drones on the map, an alert when one
// appears, and posts to a channel that Watch (here and in MeshCore Open TAK)
// puts on the map. Included by UITask.cpp after FlockYouUI.inc: it shares
// Flock You's radio layer (FlockRadio.cpp) and Team's sheets, cards, channels
// and Watch store. Decoding is GridDown Secure Messenger's (remoteid.cpp,
// BlackAtlas LLC, GPL-3.0); the glue is DroneCore.h. DRONES.md has the
// user-facing side.

#if CAP_FLOCK_YOU

#include "DroneCore.h"
#include "remoteid.h"

namespace droneui {

constexpr uint8_t kLocalChannel = 0xFF;   // a Watch item heard by this standalone itself
constexpr uint32_t kMapEveryMs = 2000;    // a drone's map spot moves at most this often

struct Cfg {
  bool armed = false;
  bool share = false;
  bool sound = true;
  char channel[32] = "";
};

static Cfg s_cfg;
static bool s_cfg_loaded = false;
static drone::AddrIds* s_ids = nullptr;
static uint32_t s_ver = 0;
static uint32_t s_posted = 0;
static char s_share_err[48] = "";
static uint32_t s_map_at[RID_MAX_TRACKS] = {0};
static uint32_t s_heard = 0, s_bad = 0;   // payloads taken / not understood
static uint32_t s_map_ver = 0;            // a drone moved on the map

static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_body = nullptr;
static lv_timer_t* s_page_timer = nullptr;
static int s_tab_i = 0;

static void toast(const char* m, int ms = 2200) { if (g_lv.task) g_lv.task->showAlert(m, ms); }

static void loadCfg() {
  if (s_cfg_loaded) return;
  s_cfg_loaded = true;
  SdNvsPrefs p;
  if (!p.begin("drones", true)) return;
  s_cfg.armed = p.getBool("armed", false);
  s_cfg.share = p.getBool("share", false);
  s_cfg.sound = p.getBool("sound", true);
  snprintf(s_cfg.channel, sizeof s_cfg.channel, "%s", p.getString("channel", "").c_str());
  p.end();
}

static void saveCfg() {
  SdNvsPrefs p;
  if (!p.begin("drones", false)) return;
  p.putBool("armed", s_cfg.armed);
  p.putBool("share", s_cfg.share);
  p.putBool("sound", s_cfg.sound);
  p.putString("channel", s_cfg.channel);
  p.end();
}

static bool ensureIds() {
  if (s_ids) return true;
  void* m = heap_caps_malloc(sizeof(drone::AddrIds), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!m) return false;
  s_ids = new (m) drone::AddrIds();
  return true;
}

static void setArmed(bool on) {
  if (on && !ensureIds()) { toast(TR("Out of memory")); on = false; }
  if (on && !s_cfg.armed) rid_init();
  s_cfg.armed = on;
  FlockRadio::setDroneWanted(on);
  saveCfg();
  s_ver++;
}

// ---- the alert channel (as Flock You: never Public) -----------------------------------
static bool channelUsable(const char* name, int* idx_out) {
  const int idx = teamui::Io::channelIdx(name);
  if (idx < 0) return false;
  if (!strcasecmp(name, "public") || teamui::threadIsPublicChannel(name)) return false;
  if (idx_out) *idx_out = idx;
  return true;
}

static bool post(const char* text) {
  int idx = -1;
  if (!s_cfg.channel[0]) { snprintf(s_share_err, sizeof s_share_err, "%s", "no channel chosen"); return false; }
  if (!channelUsable(s_cfg.channel, &idx)) { snprintf(s_share_err, sizeof s_share_err, "%s", "channel not found"); return false; }
  ChannelDetails cd;
  if (!the_mesh.getChannel(idx, cd) || strcmp(cd.name, s_cfg.channel) != 0) {
    snprintf(s_share_err, sizeof s_share_err, "%s", "channel changed");
    return false;
  }
  const char* sender = the_mesh.getNodePrefs()->node_name;
  const uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  if (!sendGroupMessageScoped(idx, cd, ts, sender && *sender ? sender : "me", text)) {
    snprintf(s_share_err, sizeof s_share_err, "%s", "send failed");
    return false;
  }
  if (g_lv.task) g_lv.task->appSentMsgToChannel(cd.name, text, the_mesh.uiLastSentFp());
  s_share_err[0] = 0;
  s_posted++;
  return true;
}

static bool shareTrack(int i, uint32_t now) {
  const RemoteIdTrack* t = rid_getTrack(i);
  char text[200];
  if (!t || !drone::formatPost(*t, text, sizeof text)) return false;
  if (!post(text)) return false;
  rid_markAlertSent(i, now);
  s_ver++;
  return true;
}

// ---- tracks -----------------------------------------------------------------------------
// A drone with a position goes on the map through the Watch store, read back
// from its own post text, so it looks the same as one posted by someone else.
static void toMap(int i, uint32_t now) {
  const RemoteIdTrack* t = rid_getTrack(i);
  if (!t || !t->hasPosition) return;
  if (s_map_at[i] && now - s_map_at[i] < kMapEveryMs) return;
  char text[200];
  if (!drone::formatPost(*t, text, sizeof text)) return;
  watchfeed::Report r;
  watchfeed::Store* w = teamui::watch();
  if (!w || !watchfeed::parse(text, r)) return;
  if (w->add(r, kLocalChannel, now)) {
    teamui::s_io.changed();
    s_map_ver++;
  }
  s_map_at[i] = now;
}

static void heard(int i, uint32_t now) {
  const RemoteIdTrack* t = rid_getTrack(i);
  if (!t) return;
  if (t->msgCount == 1) {   // new this session
    char m[64];
    snprintf(m, sizeof m, "%s %s", TR("Drone:"), t->uasId);
    toast(m, 3000);
    if (s_cfg.sound && g_lv.task && !g_lv.task->isBuzzerQuiet()) uiPlaySlot(TOUCH_SND_MEN);
    s_map_at[i] = 0;
  }
  toMap(i, now);
  s_ver++;
}

// ---- the page ------------------------------------------------------------------------------
static void pageBuild();

using teamui::lbl;
using teamui::rowCard;

static lv_obj_t* wideBtn(lv_obj_t* b, const char* t, lv_event_cb_t cb, void* ud = nullptr, uint32_t bg = 0) {
  lv_obj_t* x = lv_btn_create(b);
  styleButton(x);
  lv_obj_set_size(x, LV_PCT(100), 32);
  if (bg) teamui::fill(x, bg);
  lv_obj_add_event_cb(x, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* l = lv_label_create(x);
  lv_label_set_text(l, t);
  useChainedFont(l);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  return x;
}

static void transports(uint8_t mask, char* out, size_t cap) {
  out[0] = 0;
  size_t k = 0;
  for (uint8_t tr = RID_TRANSPORT_WIFI_BEACON; tr <= RID_TRANSPORT_BLE_LR; tr++)
    if (mask & (1u << tr)) k += (size_t)snprintf(out + k, cap - k, "%s%s", k ? "+" : "", rid_transportName(tr));
}

static void ageText(uint32_t ms, char* out, size_t cap) {
  if (ms < 60000) snprintf(out, cap, "%lus", (unsigned long)(ms / 1000));
  else snprintf(out, cap, "%lum", (unsigned long)(ms / 60000));
}

static void trackSheet(int i) {
  const RemoteIdTrack* t = rid_getTrack(i);
  if (!t) return;
  char title[48], body[420], tr[24], age[12], rb[40] = "-", op[40] = "-", sp[16];
  snprintf(title, sizeof title, "%s %s", TR("Drone"), t->uasId);
  transports(t->transportMask, tr, sizeof tr);
  ageText(millis() - t->lastSeenMs, age, sizeof age);
  if (t->hasPosition) teamui::rangeTo(t->lat, t->lon, rb, sizeof rb);
  if (t->hasOperator) teamui::rangeTo(t->opLat, t->opLon, op, sizeof op);
  drone::speedText(t->speedCms, sp, sizeof sp);
  snprintf(body, sizeof body,
           "%s: %s\n%s: %s\n%s: %s\n%s: %dm \xC2\xB7 %s: %dm\n%s: %s \xC2\xB7 %s %u\xC2\xB0\n%s: %s\n%s: %s \xC2\xB7 %ddBm \xC2\xB7 %s",
           TR("Type"), rid_uaTypeName(t->uaType), TR("Operator ID"), t->operatorId[0] ? t->operatorId : "-",
           TR("Position"), rb, TR("Height"), t->heightAglM, TR("Altitude"), t->altGeoM, TR("Speed"), sp, TR("heading"),
           (unsigned)t->headingDeg, TR("Operator"), op, TR("Heard"), tr, t->rssi, age);
  lv_obj_t* card = teamui::sheetOpen(title, body);
  if (t->hasPosition) {
    teamui::sheetBtn(card, TR("Post to the alert channel"), [](lv_event_t* e) {
      const int i = (int)(intptr_t)lv_event_get_user_data(e);
      if (shareTrack(i, millis())) toast(TR("Posted"));
      else toast(s_share_err[0] ? s_share_err : TR("Could not post"));
      teamui::sheetClose();
    }, (void*)(intptr_t)i);
  }
  teamui::sheetBtn(card, TR("Close"), [](lv_event_t*) { teamui::sheetClose(); });
}

static void statusLine(char* out, size_t cap) {
  if (!s_cfg.armed) { snprintf(out, cap, "%s", TR("Stopped. Start to listen for drones broadcasting Remote ID.")); return; }
  char w[32], b[24];
  if (FlockRadio::wifiActive()) snprintf(w, sizeof w, "Wi-Fi ch %u", FlockRadio::wifiChannel());
  else snprintf(w, sizeof w, "Wi-Fi %s", FlockRadio::wifiWhyNot());
  if (FlockRadio::bleActive()) snprintf(b, sizeof b, "BLE scan");
  else if (*FlockRadio::bleWhyNot()) snprintf(b, sizeof b, "BLE %s", FlockRadio::bleWhyNot());
  else snprintf(b, sizeof b, "BLE rest");
  snprintf(out, cap, "%s \xC2\xB7 %s \xC2\xB7 %s", TR("Listening"), w, b);
}

static void buildLive(lv_obj_t* b) {
  char line[200];
  statusLine(line, sizeof line);
  lbl(b, line, &g_font_12, COLOR_SUB);
  wideBtn(b, s_cfg.armed ? TR("Stop listening") : TR("Start listening"), [](lv_event_t*) {
    setArmed(!s_cfg.armed);
    pageBuild();
  }, nullptr, s_cfg.armed ? 0x7F1D1D : 0x2E7D32);
  if (s_cfg.share && s_share_err[0]) {
    snprintf(line, sizeof line, "%s %s: %s", TR("Alerts to"), s_cfg.channel, s_share_err);
    lbl(b, line, &g_font_12, 0xF2A33A);
  }
  const uint32_t now = millis();
  int shown = 0;
  for (int i = 0; i < RID_MAX_TRACKS; i++) {
    const RemoteIdTrack* t = rid_getTrack(i);
    if (!t) continue;
    shown++;
    const bool stale = now - t->lastSeenMs > RID_STALE_MS;
    lv_obj_t* card = rowCard(b, [](lv_event_t* e) { trackSheet((int)(intptr_t)lv_event_get_user_data(e)); },
                             (void*)(intptr_t)i);
    lv_obj_set_style_border_color(card, lv_color_hex(stale ? 0x6B7280 : 0xFDD835), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    snprintf(line, sizeof line, "%s  %s", t->uasId, rid_uaTypeName(t->uaType));
    lbl(card, line, &g_font_14, stale ? COLOR_SUB : COLOR_TEXT);
    char rb[40] = "no position", op[48] = "", age[12], sp[16];
    if (t->hasPosition) teamui::rangeTo(t->lat, t->lon, rb, sizeof rb);
    if (t->hasOperator) {
      char o[40];
      teamui::rangeTo(t->opLat, t->opLon, o, sizeof o);
      snprintf(op, sizeof op, " \xC2\xB7 %s %s", TR("Op"), o);
    }
    ageText(now - t->lastSeenMs, age, sizeof age);
    drone::speedText(t->speedCms, sp, sizeof sp);
    snprintf(line, sizeof line, "%s \xC2\xB7 h%dm \xC2\xB7 %s%s \xC2\xB7 %ddBm \xC2\xB7 %s", rb, t->heightAglM, sp, op, t->rssi, age);
    lbl(card, line, &g_font_12, COLOR_SUB);
  }
  if (!shown) {
    lbl(b, s_cfg.armed ? TR("No drones heard. Drones that broadcast Remote ID show here: most made since 2023 do, "
                            "over Wi-Fi or Bluetooth. Drones without it are not detected.")
                       : TR("No drones heard."), &g_font_12, COLOR_SUB);
    return;
  }
  wideBtn(b, TR("Clear the list"), [](lv_event_t*) {
    rid_reset();
    if (s_ids) s_ids->clear();
    memset(s_map_at, 0, sizeof s_map_at);
    s_ver++;
    pageBuild();
  });
}

static void setupToggle(lv_event_t* e) {
  switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case 0:
      if (!s_cfg.share && !s_cfg.channel[0]) { toast(TR("Pick the alert channel first")); return; }
      s_cfg.share = !s_cfg.share;
      break;
    case 1: s_cfg.sound = !s_cfg.sound; break;
    case 2: {   // others' drones on this channel on the map: Team's watched channels
      watchfeed::Store* w = teamui::watch();
      if (!w || !s_cfg.channel[0]) return;
      if (!w->setWatched(s_cfg.channel, !w->isWatched(s_cfg.channel))) { toast(TR("Watching 8 channels already")); return; }
      teamui::watchSave();
      break;
    }
  }
  saveCfg();
  pageBuild();
}

static void buildSetup(lv_obj_t* b) {
  char line[160];
  auto onoff = [](bool v) { return v ? TR("on") : TR("off"); };
  snprintf(line, sizeof line, "%s: %s", TR("Sound when a new drone appears"), onoff(s_cfg.sound));
  wideBtn(b, line, setupToggle, (void*)1);

  lbl(b, TR("Alert channel (e.g. #dronewatch; never Public):"), &g_font_12, COLOR_SUB);
  lv_obj_t* row = lv_obj_create(b);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(row, 6, LV_PART_MAIN);
  int shown = 0;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails cd;
    if (!the_mesh.getChannel(i, cd) || !cd.name[0] || !channelUsable(cd.name, nullptr)) continue;
    lv_obj_t* x = lv_btn_create(row);
    styleButton(x);
    lv_obj_set_size(x, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_pad_hor(x, 10, LV_PART_MAIN);
    if (!strcmp(cd.name, s_cfg.channel)) teamui::selected(x);
    lv_obj_add_event_cb(x, [](lv_event_t* e) {
      ChannelDetails d;
      if (!the_mesh.getChannel((int)(intptr_t)lv_event_get_user_data(e), d)) return;
      if (!strcmp(d.name, s_cfg.channel)) { s_cfg.channel[0] = 0; s_cfg.share = false; }   // tap again: none
      else snprintf(s_cfg.channel, sizeof s_cfg.channel, "%s", d.name);
      s_share_err[0] = 0;
      saveCfg();
      pageBuild();
    }, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, cd.name);
    useChainedFont(l);
    lv_obj_center(l);
    shown++;
  }
  if (!shown)
    lbl(row, TR("No channel to post on. Add one in Chats first: a hashtag channel like #dronewatch, or a private one."),
        &g_font_12, 0xF2A33A);
  snprintf(line, sizeof line, "%s: %s", TR("Post drones there"), onoff(s_cfg.share));
  wideBtn(b, line, setupToggle, (void*)0, s_cfg.share ? 0x2E7D32 : 0);
  if (s_cfg.channel[0]) {
    watchfeed::Store* w = teamui::watch();
    snprintf(line, sizeof line, "%s %s %s: %s", TR("Show"), s_cfg.channel, TR("drones from others on the map"),
             onoff(w && w->isWatched(s_cfg.channel)));
    wideBtn(b, line, setupToggle, (void*)2);
  }
  snprintf(line, sizeof line, "%s %lu", TR("Posts carry the drone's position and its operator's. Each drone at most once a "
                                          "minute, and again only after it moves 200 m. Posted so far:"),
           (unsigned long)s_posted);
  lbl(b, line, &g_font_12, COLOR_SUB);

  const FlockRadio::Diag& g = FlockRadio::diag();
  char diag[220];
  snprintf(diag, sizeof diag,
           "Remote ID from Wi-Fi %lu \xC2\xB7 BLE %lu \xC2\xB7 queue drops %lu\n"
           "Understood %lu \xC2\xB7 not %lu \xC2\xB7 tracks %d/%d",
           (unsigned long)g.rid_wifi, (unsigned long)g.rid_ble, (unsigned long)g.rid_drops, (unsigned long)s_heard,
           (unsigned long)s_bad, rid_trackCount(), RID_MAX_TRACKS);
  lbl(b, diag, &g_font_12, 0x7CB342);
  lbl(b, TR("Remote ID decoding from GridDown Secure Messenger (BlackAtlas, GPL-3.0). Cooperative only: it hears drones "
            "that broadcast Remote ID, not ones that don't. Wi-Fi listens only while the standalone is not connected "
            "to a network, and shares the radios with Flock You."),
      &g_font_12, COLOR_SUB);
}

static void pageClose();
static void pageBack() { pageClose(); }

static void pageBuild() {
  if (!s_root) return;
  if (s_body) { lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN); lv_obj_del_async(s_body); }
  s_body = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_body);
  lv_obj_set_size(s_body, LV_PCT(100), LV_PCT(100));
  lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(s_body, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_body, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
  lv_obj_t* tabs = lv_obj_create(s_body);
  lv_obj_remove_style_all(tabs);
  lv_obj_set_size(tabs, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(tabs, 6, LV_PART_MAIN);
  static const char* const names[2] = {"Live", "Setup"};
  for (int i = 0; i < 2; i++) {
    lv_obj_t* x = lv_btn_create(tabs);
    styleButton(x);
    lv_obj_set_size(x, LV_SIZE_CONTENT, 28);
    lv_obj_set_flex_grow(x, 1);
    if (i == s_tab_i) teamui::selected(x);
    lv_obj_add_event_cb(x, [](lv_event_t* e) { s_tab_i = (int)(intptr_t)lv_event_get_user_data(e); pageBuild(); },
                        LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* l = lv_label_create(x);
    lv_label_set_text(l, TR(names[i]));
    useChainedFont(l);
    lv_obj_center(l);
  }
  if (s_tab_i == 0) buildLive(s_body);
  else buildSetup(s_body);
  teamui::tint(s_body);
}

static void pageClose() {
  if (s_page_timer) { lv_timer_del(s_page_timer); s_page_timer = nullptr; }
  teamui::sheetClose();
  s_body = nullptr;
  if (s_root) { lv_obj_t* r = s_root; s_root = nullptr; appPageDeleteRootAsync(r); }
  appPageEnd(pageBack);
}

static void pageOpen() {
  loadCfg();
  if (s_root) return;
  static bool s_opened_once = false;
  if (!s_opened_once && !s_cfg.armed) setArmed(true);   // the tile is for listening
  s_opened_once = true;
  s_root = appPageCreateRoot(COLOR_BG);
  appPageBeginSlim("Drone Detector", pageBack);
  s_tab_i = 0;
  pageBuild();
  s_page_timer = lv_timer_create([](lv_timer_t*) {
    static uint32_t seen = 0, ticks = 0;
    if (s_ver != seen || (s_tab_i == 0 && (++ticks % 3) == 0)) { seen = s_ver; if (s_tab_i == 0) pageBuild(); }
  }, 2000, nullptr);
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

// ---- loop ---------------------------------------------------------------------------------------
static void loopTick() {
  const uint32_t now = millis();
  if (!s_cfg_loaded) {
    loadCfg();
    if (s_cfg.armed) {   // listening survives a reboot
      if (ensureIds()) { rid_init(); FlockRadio::setDroneWanted(true); }
      else s_cfg.armed = false;
    }
  }
  if (!s_cfg.armed || !s_ids) return;
  // FlockRadio::tick() runs in flockui::loopTick(), for both apps.
  FlockRadio::RidRaw raw;
  int budget = 8;   // the rest waits for the next pass
  while (budget-- > 0 && FlockRadio::popRemoteId(raw)) {
    const int i = drone::ingest(raw.data, raw.len, raw.transport, raw.rssi, raw.mac, now, *s_ids);
    if (i >= 0) { s_heard++; heard(i, now); }
    else s_bad++;
  }
  static uint32_t map_seen = 0, last_map = 0;
  if (s_map_ver != map_seen && now - last_map > 1500) {   // as Flock You: redraw while the map shows
    map_seen = s_map_ver;
    last_map = now;
    if (s_map_canvas) renderMapMarkers();
  }
  static uint32_t last = 0;
  if (now - last < 1000) return;
  last = now;
  rid_ageTracks(now);
  if (!s_cfg.share || !s_cfg.channel[0]) return;
  for (int i = 0; i < RID_MAX_TRACKS; i++) {   // one post per second at most
    const RemoteIdTrack* t = rid_getTrack(i);
    if (!t || !t->hasPosition || now - t->lastSeenMs > RID_STALE_MS) continue;
    if (!rid_shouldSendAlert(i, now)) continue;
    if (shareTrack(i, now) || s_share_err[0]) break;
  }
}

}  // namespace droneui

#endif  // CAP_FLOCK_YOU
