// SPDX-License-Identifier: GPL-3.0-or-later
// Flock You on this radio: the detection engine's loop, the log and camera
// files, the map layer, proximity warnings, alerts to a channel, and the app
// page. Included by UITask.cpp after TeamUI.inc (it borrows Team's sheets,
// cards and channel helpers). The rules live in FlockCore.h, the radios in
// FlockRadio.cpp. FLOCK_YOU.md has the user-facing side.

#if CAP_FLOCK_YOU

#include <new>

#include "FlockCore.h"
#include "FlockRadio.h"

namespace flockui {

using flockyou::Detection;

constexpr int kRows = 160;              // detection table (PSRAM)
constexpr int kMaxCams = 40000;         // known cameras (PSRAM, 12 bytes each)
constexpr size_t kLogRotateBytes = 512 * 1024;
constexpr size_t kLogTailBytes = 24 * 1024;   // what is read back at start: the newest few hundred rows
constexpr uint32_t kProxEveryMs = 2000;
constexpr uint32_t kProxRepeatMs = 10UL * 60000;
constexpr uint32_t kShareFreshMs = 10UL * 60000;   // a hit waits this long for a GPS fix to be shared

static const char* const kDir = "/flock-you";
static const char* const kLogRel = "/flock-you/detections.csv";
static const char* const kLogOldRel = "/flock-you/detections.old.csv";
static const char* const kCamsRel = "/flock-you/cameras.csv";
static const char* const kCamsTmpRel = "/flock-you/cameras.part";   // a download in progress
static const char* const kMarkedRel = "/flock-you/marked.csv";

static flockyou::Settings s_cfg;
static bool s_cfg_loaded = false;
static bool s_cfg_found = false;   // the app has been used on this radio
static flockyou::Table<kRows>* s_tab = nullptr;
static flockyou::CamDb s_cams;
static flockyou::Cam* s_cam_buf = nullptr;
static flockyou::SendGate s_gate;
static uint32_t s_ver = 0;       // anything the page shows changed
static uint32_t s_map_ver = 0;   // anything the map shows changed
static char s_share_err[48] = "";   // why the last post did not go out
static uint32_t s_shared_n = 0;

// Last proximity warnings, so one camera does not nag.
struct ProxRec { int32_t idx = -1; uint32_t at = 0; };
static ProxRec s_prox[8];
static int32_t s_near_idx = -1;     // nearest known camera within the radius, for the page
static double s_near_m = 0;

static void toast(const char* m, int ms = 2200) { if (g_lv.task) g_lv.task->showAlert(m, ms); }
static void chime() { if (g_lv.task && !g_lv.task->isBuzzerQuiet()) uiPlaySlot(TOUCH_SND_MEN); }

static uint32_t epochNow() {
  const uint32_t t = the_mesh.getRTCClock()->getCurrentTime();
  return t > 1700000000UL ? t : 0;
}

// A real GPS fix (not the position typed into Settings: a detection is tied to where it was heard).
static bool gpsFix(double* lat, double* lon) {
  UITask* t = g_lv.task;
  if (!t || !t->getGPSState() || !t->getGpsFix()) return false;
  *lat = t->getNodeLat();
  *lon = t->getNodeLon();
  return *lat != 0.0 || *lon != 0.0;
}

// ---- files -------------------------------------------------------------------------------
// The SD card where the radio keeps its data there, the internal store otherwise:
// the same root the Files app and Lua apps see, so cameras.csv can be dropped in
// with Files, Transfer or USB Files.
static fs::FS* dataFs(char* path, size_t cap, const char* rel) {
#if CAP_LUA_APPS
  if (fs::FS* fs = luaHostAppFs()) {
    luaHostAppPath(path, cap, rel);
    return fs;
  }
#endif
  snprintf(path, cap, "%s", rel);
  return backupInternalFs();
}

static void ensureDir(fs::FS* fs) {
  char d[96];
  dataFs(d, sizeof d, kDir);
  if (!fs->exists(d)) fs->mkdir(d);
}

static void saveCfg() {
  char buf[256];
  const size_t n = flockyou::saveSettings(s_cfg, buf, sizeof buf);
  teamui::s_io.writeFile("flockyou", (const uint8_t*)buf, n);
}

static void loadCfg() {
  if (s_cfg_loaded) return;
  s_cfg_loaded = true;
  char buf[256];
  size_t n = 0;
  if (teamui::s_io.readFile("flockyou", (uint8_t*)buf, sizeof buf - 1, &n)) {
    buf[n] = 0;
    flockyou::loadSettings(buf, s_cfg);
    s_cfg_found = true;
  }
  if (bgListenerResetOnce("flock") && s_cfg.armed) { s_cfg.armed = false; saveCfg(); }
}

static void appendLog(const Detection& d) {
  char path[96];
  fs::FS* fs = dataFs(path, sizeof path, kLogRel);
  if (!fs) return;
  WdtHeavyGuard guard;
  ensureDir(fs);
  bool fresh = !fs->exists(path);
  if (!fresh) {
    File probe = fs->open(path, "r");
    const size_t sz = probe ? probe.size() : 0;
    if (probe) probe.close();
    if (sz > kLogRotateBytes) {   // keep one old file, start a new one
      char old[96];
      dataFs(old, sizeof old, kLogOldRel);
      if (fs->exists(old)) fs->remove(old);
      fs->rename(path, old);
      fresh = true;
    }
  }
  File f = fs->open(path, fresh ? "w" : "a");
  if (!f) return;
  if (fresh) f.println(flockyou::kLogHeader);
  char line[200];
  flockyou::formatLogRow(d, line, sizeof line);
  f.println(line);
  f.close();
}

// The newest rows of the log back into the table, so History and the map
// survive a reboot. Only the tail is read: the file can be large.
static void loadLogTail() {
  char path[96];
  fs::FS* fs = dataFs(path, sizeof path, kLogRel);
  if (!fs || !s_tab || !fs->exists(path)) return;
  WdtHeavyGuard guard;
  File f = fs->open(path, "r");
  if (!f) return;
  const size_t sz = f.size();
  if (sz > kLogTailBytes) {
    f.seek(sz - kLogTailBytes);
    f.readStringUntil('\n');   // the cut line
  }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    Detection d;
    if (flockyou::parseLogRow(line.c_str(), d)) s_tab->load(d);
  }
  f.close();
}

// ---- known cameras -----------------------------------------------------------------------
// Read a few KB per loop pass, so tens of thousands of cameras never stall the UI.
enum class CamLoad : uint8_t { Idle, Cameras, Marked, Done, Missing };
static CamLoad s_cam_state = CamLoad::Idle;
static File s_cam_file;
static char s_cam_line[128];
static size_t s_cam_line_n = 0;
static int s_cams_from_file = 0, s_cams_marked = 0;
static bool s_cams_truncated = false;

static size_t fileSize(const char* rel) {
  char path[96];
  fs::FS* fs = dataFs(path, sizeof path, rel);
  if (!fs || !fs->exists(path)) return 0;
  File f = fs->open(path, "r");
  const size_t n = f ? f.size() : 0;
  if (f) f.close();
  return n;
}

// Room for what the files hold (a camera line is 20-40 bytes) plus some to mark;
// grown when a reload needs more. Cleared, so call it before loading.
static bool ensureCamBuf(int want) {
  if (want > kMaxCams) want = kMaxCams;
  if (s_cam_buf && s_cams.capacity() >= want) return true;
  if (s_cam_buf) { heap_caps_free(s_cam_buf); s_cam_buf = nullptr; s_cams.attach(nullptr, 0); }
  int cap = want;
  while (cap >= 256 && !s_cam_buf) {
    s_cam_buf = static_cast<flockyou::Cam*>(heap_caps_malloc((size_t)cap * sizeof(flockyou::Cam), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_cam_buf) cap /= 2;
  }
  if (!s_cam_buf) return false;
  s_cams.attach(s_cam_buf, cap);
  return true;
}

static bool camOpen(const char* rel) {
  char path[96];
  fs::FS* fs = dataFs(path, sizeof path, rel);
  if (!fs || !fs->exists(path)) return false;
  s_cam_file = fs->open(path, "r");
  s_cam_line_n = 0;
  return (bool)s_cam_file;
}

static void camsReload() {
  if (s_cam_file) s_cam_file.close();
  const size_t bytes = fileSize(kCamsRel) + fileSize(kMarkedRel);
  if (!ensureCamBuf((int)(bytes / 18) + 512)) { s_cam_state = CamLoad::Missing; return; }
  s_cams.clear();
  s_cams_from_file = s_cams_marked = 0;
  s_cams_truncated = false;
  s_cam_state = camOpen(kCamsRel) ? CamLoad::Cameras : (camOpen(kMarkedRel) ? CamLoad::Marked : CamLoad::Done);
}

static void camTake(const char* line) {
  flockyou::Cam c;
  if (!flockyou::parseCamLine(line, c)) return;
  if (s_cam_state == CamLoad::Marked) c.flags |= flockyou::kCamMine;
  if (!s_cams.add(c)) { s_cams_truncated = true; return; }
  if (s_cam_state == CamLoad::Marked) s_cams_marked++;
  else s_cams_from_file++;
}

static void camLoadStep() {
  if (s_cam_state != CamLoad::Cameras && s_cam_state != CamLoad::Marked) return;
  uint8_t buf[1024];
  for (int chunk = 0; chunk < 6; chunk++) {
    const int n = s_cam_file ? s_cam_file.read(buf, sizeof buf) : 0;
    if (n <= 0) {
      if (s_cam_line_n) { s_cam_line[s_cam_line_n] = 0; camTake(s_cam_line); s_cam_line_n = 0; }
      if (s_cam_file) s_cam_file.close();
      if (s_cam_state == CamLoad::Cameras && camOpen(kMarkedRel)) { s_cam_state = CamLoad::Marked; return; }
      s_cam_state = CamLoad::Done;
      s_ver++;
      s_map_ver++;
      return;
    }
    for (int i = 0; i < n; i++) {
      const char ch = (char)buf[i];
      if (ch == '\n' || ch == '\r') {
        if (s_cam_line_n) { s_cam_line[s_cam_line_n] = 0; camTake(s_cam_line); s_cam_line_n = 0; }
      } else if (s_cam_line_n + 1 < sizeof s_cam_line) {
        s_cam_line[s_cam_line_n++] = ch;
      }
    }
  }
}

static bool camsReady() { return s_cam_state == CamLoad::Done && s_cams.size() > 0; }

// ---- downloading cameras on the radio ------------------------------------------------------
// The cameras within kAreaKm of the radio, straight from OpenStreetMap's Overpass
// API over Wi-Fi: what scripts/flock_cameras.py does, without a computer. The
// answer streams through osmRowToCamLine into cameras.part, which replaces
// cameras.csv only once it is complete (marked.csv is never touched). HTTPS on
// a task of its own with mbedTLS in PSRAM, as fork updates do (ForkUpdate.inc).
// Wi-Fi sniffing steps aside whenever Wi-Fi is connected, so the two never meet.
#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION) && WADA_FORK_RELEASES
#define FLOCK_DOWNLOAD 1
#else
#define FLOCK_DOWNLOAD 0
#endif

#if FLOCK_DOWNLOAD
enum class Dl : uint8_t { Idle, Running, Done, Failed };
static volatile Dl s_dl = Dl::Idle;
static volatile int s_dl_rows = 0;          // cameras so far (the page shows it)
static char s_dl_msg[96] = "";              // the last outcome, for the page
static int32_t s_dl_lat6 = 0, s_dl_lon6 = 0;
static uint8_t s_dl_area = 1;
static bool s_dl_manual = false;            // started from the page: say how it went
static uint32_t s_dl_retry_at = 0;
static uint32_t s_dl_last_ms = 0;           // when the last one finished (no clock needed)
constexpr uint32_t kDlRetryMs = 30UL * 60000;

// Takes the HTTP body (HTTPClient::writeToStream undoes the chunking) a line at
// a time and writes cameras.csv lines, through a buffer in internal RAM (SD
// writes from PSRAM come back short).
class OsmSink : public Stream {
 public:
  explicit OsmSink(File& f) : f_(f) {}
  size_t write(uint8_t c) override { feed(&c, 1); return 1; }
  size_t write(const uint8_t* b, size_t n) override { feed(b, n); return n; }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  bool finish() {
    if (n_) take();
    return flushOut();
  }
  int rows = 0;
  bool full = false;

 private:
  File& f_;
  char line_[256];
  size_t n_ = 0;
  char out_[2048];
  size_t on_ = 0;
  bool bad_ = false;
  void feed(const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
      const char c = (char)b[i];
      if (c == '\n' || c == '\r') { if (n_) take(); }
      else if (n_ + 1 < sizeof line_) line_[n_++] = c;
    }
  }
  void take() {
    line_[n_] = 0;
    n_ = 0;
    char cam[96];
    if (full || !flockyou::osmRowToCamLine(line_, cam, sizeof cam)) return;
    const size_t l = strlen(cam);
    if (on_ + l + 1 > sizeof out_) flushOut();
    memcpy(out_ + on_, cam, l);
    on_ += l;
    out_[on_++] = '\n';
    if (++rows >= kMaxCams) full = true;
    s_dl_rows = rows;
  }
  bool flushOut() {
    if (on_ && !bad_ && f_.write((const uint8_t*)out_, on_) != on_) bad_ = true;
    on_ = 0;
    return !bad_;
  }
};

static void dlTask(void*) {
  const char* fail = nullptr;
  char detail[48] = "";
  int rows = 0;
  forkupd_dev::tlsUsePsram();
  char path[96], part[96];
  fs::FS* fs = dataFs(path, sizeof path, kCamsRel);
  dataFs(part, sizeof part, kCamsTmpRel);
  char* url = (char*)malloc(900);
  const flockyou::Box box =
      flockyou::areaBox(s_dl_lat6 / 1e6, s_dl_lon6 / 1e6, flockyou::kAreaKm[s_dl_area % flockyou::kAreaChoices]);
  if (!url || !flockyou::overpassUrl(box, url, 900)) fail = "out of memory";
  else if (!fs) fail = "no storage";
  File f;
  if (!fail) {
    ensureDir(fs);
    f = fs->open(part, "w");
    if (!f) fail = "can't write the file";
  }
  OsmSink* sink = nullptr;
  WiFiClientSecure* c = nullptr;
  HTTPClient* h = nullptr;
  if (!fail) {
    sink = new (std::nothrow) OsmSink(f);
    c = new (std::nothrow) WiFiClientSecure();
    h = new (std::nothrow) HTTPClient();
    if (!sink || !c || !h) fail = "out of memory";
  }
  if (!fail) {
    char hdr[160];
    snprintf(hdr, sizeof hdr, "# OpenStreetMap ALPR cameras (DeFlock), %u km around %.4f,%.4f, downloaded by the radio\n"
             "lat,lon,direction,label\n", flockyou::kAreaKm[s_dl_area % flockyou::kAreaChoices], s_dl_lat6 / 1e6, s_dl_lon6 / 1e6);
    f.print(hdr);
    c->setInsecure();   // no CA store on the radio; the data is public and checked line by line
    h->setReuse(false);
    h->setConnectTimeout(15000);
    h->setTimeout(65000);   // Overpass can think for a while before the first byte
    h->setUserAgent("wadamesh-flock-you/2.0");
    if (!h->begin(*c, url)) {
      fail = "can't reach Overpass";
    } else {
      const int code = h->GET();
      if (code == 200) {
        const int got = h->writeToStream(sink);
        if (got < 0) { fail = "the download broke off"; snprintf(detail, sizeof detail, " (%d)", got); }
      } else if (code == 429 || code == 504) {
        fail = "Overpass is busy";
        snprintf(detail, sizeof detail, " (%d)", code);
      } else {
        fail = code < 0 ? "can't connect to Overpass" : "Overpass refused";
        snprintf(detail, sizeof detail, " (%d)", code);
      }
      h->end();
    }
    if (!sink->finish() && !fail) fail = "storage full or failing";
    rows = sink->rows;
  }
  if (f) f.close();
  delete h;
  if (c) { c->stop(); delete c; }
  delete sink;
  free(url);
  if (!fail && rows > 0) {   // complete: it replaces the old list
    fs->remove(path);
    if (!fs->rename(part, path)) fail = "can't replace cameras.csv";
  } else if (fs) {
    fs->remove(part);
  }
  if (fail) snprintf(s_dl_msg, sizeof s_dl_msg, "%s%s", fail, detail);
  else if (!rows) snprintf(s_dl_msg, sizeof s_dl_msg, "%s", "no cameras mapped in this area");
  else s_dl_msg[0] = 0;
  s_dl_rows = rows;
  Serial.printf("[FLOCK] camera download: %d cameras %s; stack %u unused\n", rows, fail ? fail : "ok",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  s_dl = fail ? Dl::Failed : Dl::Done;
  vTaskDelete(nullptr);
}

// Where the radio is: a GPS fix, else the position set in Settings.
static bool dlPosition(double* lat, double* lon) {
  if (gpsFix(lat, lon)) return true;
  UITask* t = g_lv.task;
  if (!t) return false;
  *lat = t->getNodeLat();
  *lon = t->getNodeLon();
  return *lat != 0.0 || *lon != 0.0;
}

static bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

static bool dlStart(bool manual) {
  if (s_dl == Dl::Running) return false;
  double la, lo;
  if (!wifiUp()) { if (manual) toast(TR("Connect to Wi-Fi first")); return false; }
  if (!dlPosition(&la, &lo)) { if (manual) toast(TR("No position: get a GPS fix or set one in Settings")); return false; }
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 20 * 1024) {
    if (manual) toast(TR("Not enough memory right now"));
    s_dl_retry_at = millis() + 60000;
    return false;
  }
  s_dl_lat6 = (int32_t)lround(la * 1e6);
  s_dl_lon6 = (int32_t)lround(lo * 1e6);
  s_dl_area = s_cfg.area;
  s_dl_manual = manual;
  s_dl_rows = 0;
  s_dl = Dl::Running;
  if (xTaskCreate(dlTask, "flock_dl", 16 * 1024, nullptr, 2, nullptr) != pdPASS) {
    s_dl = Dl::Idle;
    if (manual) toast(TR("Not enough memory right now"));
    s_dl_retry_at = millis() + 60000;
    return false;
  }
  s_ver++;
  return true;
}

static void dlTick(uint32_t now) {
  if (s_dl == Dl::Done || s_dl == Dl::Failed) {   // the task finished: take the result on this task
    const bool ok = s_dl == Dl::Done;
    s_dl = Dl::Idle;
    s_dl_last_ms = now;
    char t[120];
    if (ok) {
      s_cfg.dl_lat_e6 = s_dl_lat6;
      s_cfg.dl_lon_e6 = s_dl_lon6;
      s_cfg.dl_at = epochNow();
      saveCfg();
      if (s_dl_rows > 0) camsReload();
      if (s_dl_rows > 0) snprintf(t, sizeof t, "%s %d", TR("Cameras updated:"), (int)s_dl_rows);
      else snprintf(t, sizeof t, "%s", TR("No cameras mapped around here"));
      if (s_dl_manual || s_dl_rows > 0) toast(t, 2400);
    } else {
      s_dl_retry_at = now + kDlRetryMs;
      if (s_dl_manual) {
        snprintf(t, sizeof t, "%s: %s", TR("Camera download failed"), s_dl_msg);
        toast(t, 3000);
      }
    }
    s_ver++;
    s_map_ver++;
    return;
  }
  static uint32_t last = 0;
  if (s_dl == Dl::Running || now - last < 30000) return;
  last = now;
  if (!s_cfg.auto_dl || (int32_t)(now - s_dl_retry_at) < 0 || !wifiUp()) return;
  double la, lo;
  if (!dlPosition(&la, &lo) || !flockyou::downloadDue(s_cfg, la, lo, epochNow())) return;
  dlStart(false);
}
#endif  // FLOCK_DOWNLOAD

// ---- the alert channel -------------------------------------------------------------------
static bool channelUsable(const char* name, int* idx_out) {
  const int idx = teamui::Io::channelIdx(name);
  if (idx < 0) return false;
  if (!strcasecmp(name, "public") || teamui::threadIsPublicChannel(name)) return false;   // never spam Public
  if (idx_out) *idx_out = idx;
  return true;
}

// Post `text` on the alert channel, the way the chat composer does (its region
// scope, the slot's radio routing) so it also shows in that channel's thread.
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
  s_shared_n++;
  return true;
}

static bool shareDetection(Detection& d) {
  char text[160];
  flockyou::formatChannelAlert(d, text, sizeof text);
  if (!post(text)) return false;
  d.sent = true;
  s_gate.sent(d.alert.mac, millis());
  s_ver++;
  return true;
}

// Automatic posts: confident, positioned, fresh, not posted before, within the limit.
static void shareTick(uint32_t now) {
  if (!s_cfg.share || !s_cfg.channel[0] || !s_tab) return;
  for (int i = 0; i < s_tab->n; i++) {
    Detection& d = s_tab->rows[i];
    if (d.historical || d.sent || !d.has_gps) continue;
    if (flockyou::confidence(d.alert.method) < s_cfg.share_conf) continue;
    if (now - d.last_ms > kShareFreshMs) continue;
    if (!s_gate.allow(d.alert.mac, now)) continue;
    shareDetection(d);
    return;   // one per pass; the gate spaces the rest out
  }
}

// ---- detections ----------------------------------------------------------------------------
static bool ensureTable() {
  if (s_tab) return true;
  void* m = heap_caps_malloc(sizeof(flockyou::Table<kRows>), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!m) return false;
  s_tab = new (m) flockyou::Table<kRows>();
  loadLogTail();
  return true;
}

static void hear(const flockyou::Alert& a, uint32_t now) {
  bool is_new = false;
  Detection& d = s_tab->hear(a, now, epochNow(), &is_new);
  double la, lo;
  const bool had = d.has_gps;
  if (gpsFix(&la, &lo)) {
    // The strongest reading is the best guess of where the device is.
    if (!d.has_gps || is_new || a.rssi >= d.alert.rssi - 3) {
      d.lat = la;
      d.lon = lo;
      d.has_gps = true;
    }
  }
  if (d.has_gps && (!had || is_new)) s_map_ver++;
  if (d.count == 1) {   // new this session: log it, and say so when it is a real one
    appendLog(d);
    if (flockyou::confidence(d.alert.method) >= flockyou::kConfMed) {
      char m[80];
      snprintf(m, sizeof m, "%s %s %ddBm", d.alert.method == flockyou::Method::BleRaven ? TR("Raven sensor") : TR("Flock device"),
               flockyou::methodLabel(d.alert.method), d.alert.rssi);
      toast(m, 2600);
      chime();
    }
  }
  s_ver++;
}

static void proximityTick(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < kProxEveryMs) return;
  last = now;
  const int32_t was = s_near_idx;
  s_near_idx = -1;
  double la, lo;
  if (!s_cfg.proximity || !camsReady() || !gpsFix(&la, &lo)) {
    if (was != s_near_idx) s_ver++;
    return;
  }
  const double radius = flockyou::kRadiusM[s_cfg.radius % flockyou::kRadiusChoices];
  double dm = 0;
  const int idx = s_cams.nearest(la, lo, radius, &dm);
  s_near_idx = idx;
  s_near_m = dm;
  if (idx != was) s_ver++;
  if (idx < 0) return;
  ProxRec* slot = &s_prox[0];
  for (ProxRec& r : s_prox) {
    if (r.idx == idx && now - r.at < kProxRepeatMs) { r.at = now; return; }   // still near it: quiet
    if (now - r.at > now - slot->at) slot = &r;
  }
  slot->idx = idx;
  slot->at = now;
  const flockyou::Cam& c = s_cams.at(idx);
  const double cl = c.lat_e6 / 1e6, co = c.lon_e6 / 1e6;
  char brg[16];
  watchfeed::bearingLabel((float)flockyou::bearingDeg(la, lo, cl, co), brg, sizeof brg);
  const bool sees = c.dir >= 0 && flockyou::facing(c.dir, flockyou::bearingDeg(cl, co, la, lo));
  char m[96];
  snprintf(m, sizeof m, "%s %.0f m %s%s", TR("Flock camera"), dm, brg, sees ? TR(", facing you") : "");
  toast(m, 3200);
  chime();
}

// ---- map layer ---------------------------------------------------------------------------
constexpr int kMapObjs = 180;
static auto& s_objs = psramStatic<lv_obj_t*[kMapObjs]>();
static int s_obj_n = 0;
struct Hit { int16_t x, y; uint8_t kind; int32_t idx; };   // kind 0 detection, 1 camera, 2 cluster
static auto& s_hits = psramStatic<Hit[kMapObjs]>();
static int s_hit_n = 0;

static bool track(lv_obj_t* o) {
  if (s_obj_n < kMapObjs) { s_objs[s_obj_n++] = o; return true; }
  lv_obj_del(o);
  return false;
}
static void hit(int sx, int sy, uint8_t kind, int32_t idx) {
  if (s_hit_n < kMapObjs) s_hits[s_hit_n++] = {(int16_t)sx, (int16_t)sy, kind, idx};
}

static void mapFree() {
  for (int i = 0; i < s_obj_n; i++) if (s_objs[i]) lv_obj_del(s_objs[i]);
  s_obj_n = 0;
  s_hit_n = 0;
}

static lv_obj_t* dot(lv_obj_t* parent, int sx, int sy, int size, uint32_t fill, uint32_t border, int radius, lv_opa_t opa) {
  if (s_obj_n >= kMapObjs) return nullptr;
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
  track(d);
  return d;
}

static void glyph(lv_obj_t* d, const char* g, uint32_t color, float deg = 0) {
  if (!d) return;
  lv_obj_t* l = lv_label_create(d);
  lv_label_set_text(l, g);
  lv_obj_set_style_text_font(l, &map_icons_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(l);
  if (deg != 0) {
    lv_obj_set_style_transform_pivot_x(l, 7, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(l, 7, LV_PART_MAIN);
    lv_obj_set_style_transform_angle(l, (int16_t)((int)(deg * 10) % 3600), LV_PART_MAIN);
  }
}

static void text(lv_obj_t* parent, int sx, int sy, const char* t, uint32_t color, bool centred = false) {
  if (s_obj_n >= kMapObjs) return;
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, &g_font_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  if (centred) {
    lv_obj_update_layout(l);
    lv_obj_set_pos(l, sx - lv_obj_get_width(l) / 2, sy - lv_obj_get_height(l) / 2);
  } else {
    lv_obj_set_style_bg_color(l, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(l, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(l, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(l, 3, LV_PART_MAIN);
    lv_obj_set_pos(l, sx + 9, sy - 8);
  }
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  track(l);
}

constexpr uint32_t kCamRgb = 0xE53935;    // a known camera
constexpr uint32_t kMineRgb = 0xD81B60;   // one marked on this radio
constexpr uint32_t kWifiRgb = 0xE8A33D;   // heard over Wi-Fi
constexpr uint32_t kBleRgb = 0x15B6A6;    // heard over BLE

static void drawCameras(lv_obj_t* parent, double cwx, double cwy) {
  // The visible box in degrees, with a margin, to skip most of the file cheaply.
  double la_top, lo_left, la_bot, lo_right;
  worldPxToLatLon(cwx - k_map_canvas_w / 2 - 16, cwy - k_map_canvas_h / 2 - 16, s_map_zoom, &la_top, &lo_left);
  worldPxToLatLon(cwx + k_map_canvas_w / 2 + 16, cwy + k_map_canvas_h / 2 + 16, s_map_zoom, &la_bot, &lo_right);
  const int32_t la_hi = (int32_t)(la_top * 1e6), la_lo = (int32_t)(la_bot * 1e6);
  const int32_t lo_lo = (int32_t)(lo_left * 1e6), lo_hi = (int32_t)(lo_right * 1e6);
  // Cluster on a grid when zoomed out: one marker a cell, with the count.
  const bool cluster = s_map_zoom < 15;
  constexpr int kCell = 26;
  const int gw = k_map_canvas_w / kCell + 2, gh = k_map_canvas_h / kCell + 2;
  static auto& grid = psramStatic<int32_t[1536]>();     // first camera in the cell, -1 none
  static auto& count = psramStatic<uint16_t[1536]>();
  const bool can_cluster = gw * gh <= 1536;
  if (cluster && can_cluster)
    for (int i = 0; i < gw * gh; i++) { grid[i] = -1; count[i] = 0; }
  int sx, sy;
  for (int i = 0; i < s_cams.size(); i++) {
    const flockyou::Cam& c = s_cams.at(i);
    if (c.lat_e6 > la_hi || c.lat_e6 < la_lo || c.lon_e6 < lo_lo || c.lon_e6 > lo_hi) continue;
    if (!teamui::toScreen(c.lat_e6 / 1e6, c.lon_e6 / 1e6, cwx, cwy, &sx, &sy)) continue;
    if (cluster && can_cluster) {
      const int gx = (sx + 12) / kCell, gy = (sy + 12) / kCell;
      if (gx < 0 || gy < 0 || gx >= gw || gy >= gh) continue;
      const int k = gy * gw + gx;
      if (grid[k] < 0 || (c.flags & flockyou::kCamMine)) grid[k] = i;
      if (count[k] < 0xFFFF) count[k]++;
      continue;
    }
    if (s_obj_n >= kMapObjs - 2) break;
    const uint32_t col = (c.flags & flockyou::kCamMine) ? kMineRgb : kCamRgb;
    lv_obj_t* d = dot(parent, sx, sy, 20, 0x101418, col, 5, LV_OPA_COVER);
    glyph(d, mapicons::kVideocam, col);
    // Which way it looks: an arrow just outside the tile.
    if (c.dir >= 0 && s_obj_n < kMapObjs - 2) {
      const float r = c.dir * 0.0174533f;
      const int hx = sx + (int)(15 * sinf(r)), hy = sy - (int)(15 * cosf(r));
      lv_obj_t* h = dot(parent, hx, hy, 14, 0x000000, 0x000000, 0, LV_OPA_TRANSP);
      if (h) lv_obj_set_style_border_width(h, 0, LV_PART_MAIN);
      glyph(h, mapicons::kNavigation, col, (float)c.dir);
    }
    hit(sx, sy, 1, i);
  }
  if (!cluster || !can_cluster) return;
  for (int k = 0; k < gw * gh && s_obj_n < kMapObjs - 2; k++) {
    if (grid[k] < 0) continue;
    const flockyou::Cam& c = s_cams.at(grid[k]);
    if (!teamui::toScreen(c.lat_e6 / 1e6, c.lon_e6 / 1e6, cwx, cwy, &sx, &sy)) continue;
    const uint32_t col = (c.flags & flockyou::kCamMine) ? kMineRgb : kCamRgb;
    if (count[k] == 1) {
      lv_obj_t* d = dot(parent, sx, sy, 18, 0x101418, col, 5, LV_OPA_COVER);
      glyph(d, mapicons::kVideocam, col);
      hit(sx, sy, 1, grid[k]);
    } else {
      const int size = count[k] < 10 ? 20 : count[k] < 100 ? 24 : 28;
      dot(parent, sx, sy, size, col, 0xFFFFFF, LV_RADIUS_CIRCLE, LV_OPA_80);
      char n[8];
      snprintf(n, sizeof n, "%u", count[k] > 999 ? 999 : count[k]);
      text(parent, sx, sy, n, 0xFFFFFF, true);
      hit(sx, sy, 2, grid[k]);
    }
  }
}

static void drawDetections(lv_obj_t* parent, double cwx, double cwy) {
  if (!s_tab) return;
  int sx, sy;
  for (int i = 0; i < s_tab->n && s_obj_n < kMapObjs - 2; i++) {
    const Detection& d = s_tab->rows[i];
    if (!d.has_gps || !teamui::toScreen(d.lat, d.lon, cwx, cwy, &sx, &sy)) continue;
    const bool wifi = d.alert.source == flockyou::Source::Wifi;
    const uint32_t col = wifi ? kWifiRgb : kBleRgb;
    const uint8_t conf = flockyou::confidence(d.alert.method);
    const lv_opa_t opa = d.historical ? LV_OPA_50 : conf >= flockyou::kConfMed ? LV_OPA_COVER : LV_OPA_70;
    lv_obj_t* o = dot(parent, sx, sy, conf >= flockyou::kConfHigh ? 22 : 16, col, d.historical ? 0x5F6368 : 0xFFFFFF,
                      LV_RADIUS_CIRCLE, opa);
    glyph(o, wifi ? mapicons::kVideocam : mapicons::kSensors, 0x000000);
    if (!d.historical && s_map_zoom >= 14) {
      char l[24];
      snprintf(l, sizeof l, "%ddBm", d.alert.rssi);
      text(parent, sx, sy, l, 0xFFE0B2);
    }
    hit(sx, sy, 0, i);
  }
}

}  // namespace flockui

// Called from renderMapMarkers, before Team's layer (teammates draw on top).
static void flockDrawMapOverlay(lv_obj_t* parent, double cwx, double cwy) {
  using namespace flockui;
  mapFree();
  if (!s_cfg_loaded) return;
  if (s_cfg.show_cams && s_cams.size() > 0) drawCameras(parent, cwx, cwy);
  if (s_cfg.show_hits) drawDetections(parent, cwx, cwy);
}
static void flockFreeMapOverlay() { flockui::mapFree(); }

namespace flockui {

// ---- sheets --------------------------------------------------------------------------------
static int s_sel = -1;
static double s_mark_lat = 0, s_mark_lon = 0;
static void markSheet(double lat, double lon);

static void detectionSheet(int i) {
  if (!s_tab || i < 0 || i >= s_tab->n) return;
  s_sel = i;
  const Detection& d = s_tab->rows[i];
  static const char* const confName[4] = {"", "low", "medium", "high"};
  char body[260], where[48] = "", ago[24] = "", ch[12] = "";
  if (d.alert.channel) snprintf(ch, sizeof ch, " \xC2\xB7 ch %u", d.alert.channel);
  if (d.has_gps) teamui::rangeTo(d.lat, d.lon, where, sizeof where);
  if (!d.historical) snprintf(ago, sizeof ago, "%lu min ago", (unsigned long)((millis() - d.last_ms) / 60000));
  else if (d.epoch && epochNow() > d.epoch) snprintf(ago, sizeof ago, "%lu h ago (log)", (unsigned long)((epochNow() - d.epoch) / 3600));
  else snprintf(ago, sizeof ago, "earlier (log)");
  snprintf(body, sizeof body, "%s%s%s\n%s \xC2\xB7 %s confidence\n%ddBm%s \xC2\xB7 heard %ux \xC2\xB7 %s\n%s%s",
           d.alert.mac, d.alert.name[0] ? " \xC2\xB7 " : "", d.alert.name, flockyou::methodLabel(d.alert.method),
           confName[flockyou::confidence(d.alert.method) & 3], d.alert.rssi, ch, d.count, ago,
           d.has_gps ? where : "no GPS position", d.sent ? "\nPosted to the alert channel" : "");
  lv_obj_t* card = teamui::sheetOpen(d.alert.method == flockyou::Method::BleRaven ? TR("Raven sensor")
                                     : d.alert.source == flockyou::Source::Wifi ? TR("Flock camera (Wi-Fi)")
                                                                              : TR("Flock device (BLE)"), body);
  if (d.has_gps) {
    teamui::sheetBtn(card, TR("Map"), [](lv_event_t*) {
      const Detection* x = (s_tab && s_sel >= 0 && s_sel < s_tab->n) ? &s_tab->rows[s_sel] : nullptr;
      teamui::sheetClose();
      if (x && x->has_gps) teamui::showOnMap(x->lat, x->lon);
    });
    teamui::sheetBtn(card, TR("Mark as camera"), [](lv_event_t*) {
      const Detection* x = (s_tab && s_sel >= 0 && s_sel < s_tab->n) ? &s_tab->rows[s_sel] : nullptr;
      teamui::sheetClose();
      if (x && x->has_gps) markSheet(x->lat, x->lon);
    });
    if (s_cfg.channel[0] && !d.sent) {
      teamui::sheetBtn(card, TR("Post"), [](lv_event_t*) {
        Detection* x = (s_tab && s_sel >= 0 && s_sel < s_tab->n) ? &s_tab->rows[s_sel] : nullptr;
        teamui::sheetClose();
        if (!x) return;
        if (shareDetection(*x)) toast(TR("Posted"));
        else { char m[80]; snprintf(m, sizeof m, "%s: %s", TR("Not posted"), s_share_err); toast(m); }
      }, nullptr, 0xE53935);
    }
  }
  teamui::sheetBtn(card, TR("Close"), [](lv_event_t*) { teamui::sheetClose(); });
}

static void saveMarked(double lat, double lon, int dir) {
  flockyou::Cam c;
  c.lat_e6 = (int32_t)lround(lat * 1e6);
  c.lon_e6 = (int32_t)lround(lon * 1e6);
  c.dir = (int16_t)dir;
  c.flags = flockyou::kCamMine;
  double dm;
  if (s_cam_state == CamLoad::Done && s_cams.nearest(lat, lon, 15, &dm) >= 0) {
    toast(TR("A camera is already marked there"));
    return;
  }
  char path[96], line[80];
  fs::FS* fs = dataFs(path, sizeof path, kMarkedRel);
  if (fs) {
    WdtHeavyGuard guard;
    ensureDir(fs);
    File f = fs->open(path, fs->exists(path) ? "a" : "w");
    if (f) {
      char label[24];
      snprintf(label, sizeof label, "%lu", (unsigned long)epochNow());
      flockyou::formatCamLine(c, label, line, sizeof line);
      f.println(line);
      f.close();
    }
  }
  if (!s_cam_buf) ensureCamBuf(512);
  if (s_cams.add(c)) s_cams_marked++;
  s_map_ver++;
  s_ver++;
  if (s_cfg.share && s_cfg.share_marked && s_cfg.channel[0]) {
    char t[160];
    flockyou::formatMarkedAlert(lat, lon, dir, t, sizeof t);
    if (post(t)) { toast(TR("Camera marked and posted")); return; }
  }
  toast(TR("Camera marked"));
}

static int s_mark_dir = -1;
static void markSheet(double lat, double lon) {
  s_mark_lat = lat;
  s_mark_lon = lon;
  char body[64];
  snprintf(body, sizeof body, "%.5f, %.5f\n%s", lat, lon, TR("Which way does it face?"));
  lv_obj_t* card = teamui::sheetOpen(TR("Mark a Flock camera"), body);
  static const char* const names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  for (int k = 0; k < 8; k++) {
    teamui::sheetBtn(card, names[k], [](lv_event_t* e) {
      s_mark_dir = (int)(intptr_t)lv_event_get_user_data(e) * 45;
      teamui::sheetClose();
      saveMarked(s_mark_lat, s_mark_lon, s_mark_dir);
    }, (void*)(intptr_t)k);
  }
  teamui::sheetBtn(card, TR("Not sure"), [](lv_event_t*) {
    teamui::sheetClose();
    saveMarked(s_mark_lat, s_mark_lon, -1);
  }, nullptr, kMineRgb);
  teamui::sheetBtn(card, TR("Cancel"), [](lv_event_t*) { teamui::sheetClose(); });
}

static void cameraSheet(int idx, bool cluster) {
  if (idx < 0 || idx >= s_cams.size()) return;
  const flockyou::Cam& c = s_cams.at(idx);
  const double la = c.lat_e6 / 1e6, lo = c.lon_e6 / 1e6;
  if (cluster) {   // a cluster: zoom in on it
    s_map_center_lat = la;
    s_map_center_lon = lo;
    s_map_view_inited = true;
    s_map_zoom = (uint8_t)(s_map_zoom + 2 > k_map_zoom_max ? k_map_zoom_max : s_map_zoom + 2);
    renderMapTiles();
    renderMapMarkers();
    refreshMapInfoLabel();
    return;
  }
  s_mark_lat = la;
  s_mark_lon = lo;
  char body[160], rb[40], dir[24] = "direction unknown";
  teamui::rangeTo(la, lo, rb, sizeof rb);
  if (c.dir >= 0) {
    char b[16];
    watchfeed::bearingLabel((float)c.dir, b, sizeof b);
    snprintf(dir, sizeof dir, "faces %s", b);
  }
  snprintf(body, sizeof body, "%s \xC2\xB7 %s\n%s\n%.5f, %.5f", (c.flags & flockyou::kCamMine) ? "marked on this radio" : "OpenStreetMap",
           dir, rb, la, lo);
  lv_obj_t* card = teamui::sheetOpen(TR("Flock camera"), body);
  teamui::sheetBtn(card, TR("Hound"), [](lv_event_t*) {
    teamui::sheetClose();
    if (teamui::core()) teamui::s_core->houndPoint(s_mark_lat, s_mark_lon, "Flock camera");
    teamui::houndRefresh();
  }, nullptr, 0xF2A33A);
  if (s_cfg.channel[0]) {
    teamui::sheetBtn(card, TR("Post"), [](lv_event_t*) {
      teamui::sheetClose();
      char t[160];
      flockyou::formatMarkedAlert(s_mark_lat, s_mark_lon, -1, t, sizeof t);
      if (post(t)) toast(TR("Posted"));
      else { char m[80]; snprintf(m, sizeof m, "%s: %s", TR("Not posted"), s_share_err); toast(m); }
    }, nullptr, 0xE53935);
  }
  teamui::sheetBtn(card, TR("Close"), [](lv_event_t*) { teamui::sheetClose(); });
}

}  // namespace flockui

// Map tap: true when it hit a camera or a detection.
static bool flockMapTap(lv_point_t p) {
  using namespace flockui;
  if (!s_hit_n || !s_map_canvas) return false;
  lv_area_t a;
  lv_obj_get_coords(s_map_pan_layer ? s_map_pan_layer : s_map_canvas, &a);
  const int lx = p.x - a.x1, ly = p.y - a.y1;
  int best = -1, best_d = 16 * 16;
  for (int i = 0; i < s_hit_n; i++) {
    const int dx = s_hits[i].x - lx, dy = s_hits[i].y - ly, d = dx * dx + dy * dy;
    if (d <= best_d && (best < 0 || s_hits[i].kind <= s_hits[best].kind)) { best = i; best_d = d; }
  }
  if (best < 0) return false;
  if (s_hits[best].kind == 0) detectionSheet((int)s_hits[best].idx);
  else cameraSheet((int)s_hits[best].idx, s_hits[best].kind == 2);
  return true;
}

// Map long-press: mark a camera there (when Flock You is in use and Team did not take it).
static bool flockMapLongPress(lv_point_t p) {
  using namespace flockui;
  if (!s_cfg_loaded || !s_map_canvas || (!s_cfg.armed && s_cams.size() == 0)) return false;
  lv_area_t a;
  lv_obj_get_coords(s_map_canvas, &a);
  double cwx, cwy, lat, lon;
  latLonToWorldPx(s_map_center_lat, s_map_center_lon, s_map_zoom, &cwx, &cwy);
  worldPxToLatLon(cwx + (p.x - a.x1) - k_map_canvas_w / 2, cwy + (p.y - a.y1) - k_map_canvas_h / 2, s_map_zoom, &lat, &lon);
  markSheet(lat, lon);
  return true;
}

namespace flockui {

// ---- the page -------------------------------------------------------------------------------
static lv_obj_t* s_root = nullptr;
static lv_obj_t* s_body = nullptr;
static int s_tab_i = 0;   // 0 live, 1 cameras, 2 setup
static lv_timer_t* s_page_timer = nullptr;
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

static void setArmed(bool on) {
  if (on && !ensureTable()) { toast(TR("Out of memory")); on = false; }
  s_cfg.armed = on;
  FlockRadio::setWanted(on);
  saveCfg();
  s_ver++;
}

static void statusLine(char* out, size_t cap) {
  double la, lo;
  const bool fix = gpsFix(&la, &lo);
  if (!s_cfg.armed) { snprintf(out, cap, "%s", TR("Stopped. Start to listen for Flock devices.")); return; }
  char w[32], b[24];
  if (FlockRadio::wifiActive()) snprintf(w, sizeof w, "Wi-Fi ch %u", FlockRadio::wifiChannel());
  else snprintf(w, sizeof w, "Wi-Fi %s", FlockRadio::wifiWhyNot());
  if (FlockRadio::bleActive()) snprintf(b, sizeof b, "BLE scan");
  else if (*FlockRadio::bleWhyNot()) snprintf(b, sizeof b, "BLE %s", FlockRadio::bleWhyNot());
  else snprintf(b, sizeof b, "BLE rest");
  snprintf(out, cap, "%s \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 %s", TR("Listening"), w, b, fix ? "GPS fix" : "no GPS fix");
}

static void buildLive(lv_obj_t* b) {
  char line[200];
  statusLine(line, sizeof line);
  lbl(b, line, &g_font_12, COLOR_SUB);
  wideBtn(b, s_cfg.armed ? TR("Stop listening") : TR("Start listening"), [](lv_event_t*) {
    setArmed(!s_cfg.armed);
    pageBuild();
  }, nullptr, s_cfg.armed ? 0x7F1D1D : 0x2E7D32);
  if (s_near_idx >= 0 && s_near_idx < s_cams.size()) {
    const flockyou::Cam& c = s_cams.at(s_near_idx);
    char rb[40];
    teamui::rangeTo(c.lat_e6 / 1e6, c.lon_e6 / 1e6, rb, sizeof rb);
    snprintf(line, sizeof line, LV_SYMBOL_WARNING " %s  %s", TR("Known camera"), rb);
    lv_obj_t* card = rowCard(b, [](lv_event_t*) { cameraSheet(s_near_idx, false); }, nullptr);
    lv_obj_set_style_border_color(card, lv_color_hex(kCamRgb), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lbl(card, line, &g_font_14, 0xFF8A80);
  }
  if (s_cfg.share && s_share_err[0]) {
    snprintf(line, sizeof line, "%s %s: %s", TR("Alerts to"), s_cfg.channel, s_share_err);
    lbl(b, line, &g_font_12, 0xF2A33A);
  }
  if (!s_tab || !s_tab->n) {
    lbl(b, s_cfg.armed ? TR("Nothing heard yet. Flock cameras probe for Wi-Fi every few minutes; Raven sensors advertise over BLE.")
                       : TR("No detections logged."), &g_font_12, COLOR_SUB);
    return;
  }
  // Newest first: this session's, then the log's.
  int shown = 0;
  for (int pass = 0; pass < 2 && shown < 14; pass++) {
    for (int i = s_tab->n - 1; i >= 0 && shown < 14; i--) {
      const Detection& d = s_tab->rows[i];
      if (d.historical != (pass == 1)) continue;
      if (pass == 1 && shown == s_tab->liveCount()) lbl(b, TR("From the log:"), &g_font_12, COLOR_SUB);
      shown++;
      const bool wifi = d.alert.source == flockyou::Source::Wifi;
      lv_obj_t* card = rowCard(b, [](lv_event_t* e) { detectionSheet((int)(intptr_t)lv_event_get_user_data(e)); },
                               (void*)(intptr_t)i);
      lv_obj_set_style_border_color(card, lv_color_hex(wifi ? kWifiRgb : kBleRgb), LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
      snprintf(line, sizeof line, "%s  %s", wifi ? "Wi-Fi" : "BLE", d.alert.name[0] ? d.alert.name : d.alert.mac);
      lbl(card, line, &g_font_14, d.historical ? COLOR_SUB : COLOR_TEXT);
      char rb[40] = "no GPS";
      if (d.has_gps) teamui::rangeTo(d.lat, d.lon, rb, sizeof rb);
      snprintf(line, sizeof line, "%s \xC2\xB7 %ddBm \xC2\xB7 x%u \xC2\xB7 %s%s", flockyou::methodLabel(d.alert.method), d.alert.rssi,
               d.count, rb, d.sent ? " \xC2\xB7 posted" : "");
      lbl(card, line, &g_font_12, COLOR_SUB);
    }
  }
  wideBtn(b, TR("Clear this session"), [](lv_event_t*) {
    if (s_tab) s_tab->clearLive();
    s_ver++;
    s_map_ver++;
    pageBuild();
  });
}

static void buildCameras(lv_obj_t* b) {
  char line[220], path[96];
  dataFs(path, sizeof path, kCamsRel);
  switch (s_cam_state) {
    case CamLoad::Idle: snprintf(line, sizeof line, "%s", TR("Known cameras are not loaded.")); break;
    case CamLoad::Cameras: case CamLoad::Marked:
      snprintf(line, sizeof line, "%s %d...", TR("Loading cameras:"), s_cams.size()); break;
    case CamLoad::Missing: snprintf(line, sizeof line, "%s", TR("Not enough memory for the camera list.")); break;
    case CamLoad::Done:
      snprintf(line, sizeof line, "%d %s \xC2\xB7 %d %s%s", s_cams_from_file, TR("from OpenStreetMap"), s_cams_marked,
               TR("marked here"), s_cams_truncated ? TR(" \xC2\xB7 list full, some left out") : "");
      break;
  }
  lbl(b, line, &g_font_14, COLOR_TEXT);
  if (s_cam_state == CamLoad::Done && !s_cams_from_file) {
#if FLOCK_DOWNLOAD
    snprintf(line, sizeof line, "%s", TR("No camera list yet. Download the cameras around you below, or put a list made by "
                                         "scripts/flock_cameras.py in"));
    lbl(b, line, &g_font_12, 0xF2A33A);
    lbl(b, path, &g_font_12, COLOR_SUB);
#else
    snprintf(line, sizeof line, "%s %s\n%s", TR("No camera list yet. Put DeFlock's cameras for your area in"), path,
             TR("(scripts/flock_cameras.py makes it from OpenStreetMap; copy it over with Files, Transfer or USB Files)."));
    lbl(b, line, &g_font_12, 0xF2A33A);
#endif
  }
#if FLOCK_DOWNLOAD
  {
    const unsigned km = flockyou::kAreaKm[s_cfg.area % flockyou::kAreaChoices];
    if (s_dl == Dl::Running) {
      snprintf(line, sizeof line, "%s %d", TR("Downloading cameras from OpenStreetMap..."), (int)s_dl_rows);
      lbl(b, line, &g_font_12, 0x4FC3F7);
    } else if (s_dl_msg[0]) {
      snprintf(line, sizeof line, "%s: %s", TR("Last download"), s_dl_msg);
      lbl(b, line, &g_font_12, 0xF2A33A);
    } else if (s_cfg.dl_lat_e6 || s_cfg.dl_lon_e6) {
      char rb[40];
      teamui::rangeTo(s_cfg.dl_lat_e6 / 1e6, s_cfg.dl_lon_e6 / 1e6, rb, sizeof rb);
      const uint32_t now = epochNow();
      char when[32] = "";
      if (now && s_cfg.dl_at && now >= s_cfg.dl_at) {
        const uint32_t h = (now - s_cfg.dl_at) / 3600;
        if (h < 1) snprintf(when, sizeof when, " \xC2\xB7 %s", TR("just now"));
        else if (h < 48) snprintf(when, sizeof when, " \xC2\xB7 %luh %s", (unsigned long)h, TR("ago"));
        else snprintf(when, sizeof when, " \xC2\xB7 %lud %s", (unsigned long)(h / 24), TR("ago"));
      }
      snprintf(line, sizeof line, "%s %s%s", TR("Downloaded around a point"), rb, when);
      lbl(b, line, &g_font_12, COLOR_SUB);
    }
    snprintf(line, sizeof line, "%s %u km %s", TR("Download cameras within"), km, TR("now"));
    wideBtn(b, line, [](lv_event_t*) { dlStart(true); pageBuild(); }, nullptr, s_dl == Dl::Running ? 0 : kMineRgb);
    snprintf(line, sizeof line, "%s: %s", TR("Keep them current over Wi-Fi"), s_cfg.auto_dl ? TR("on") : TR("off"));
    wideBtn(b, line, [](lv_event_t*) {
      s_cfg.auto_dl = !s_cfg.auto_dl;
      s_dl_retry_at = millis();
      saveCfg();
      pageBuild();
    }, nullptr, s_cfg.auto_dl ? 0x2E7D32 : 0);
    snprintf(line, sizeof line, "%s: %u km %s", TR("Area"), km, TR("around the radio"));
    wideBtn(b, line, [](lv_event_t*) {
      s_cfg.area = (uint8_t)((s_cfg.area + 1) % flockyou::kAreaChoices);
      saveCfg();
      pageBuild();
    });
    lbl(b, TR("From OpenStreetMap (DeFlock's mapping) over Wi-Fi. When on, it downloads again once the radio moves "
              "half the area away, or after a week. It replaces cameras.csv; cameras you mark stay in marked.csv."),
        &g_font_12, COLOR_SUB);
  }
#endif
  double la, lo;
  if (camsReady() && gpsFix(&la, &lo)) {
    double dm = 0;
    const int i = s_cams.nearest(la, lo, 50000, &dm);
    if (i >= 0) {
      char rb[40];
      teamui::rangeTo(s_cams.at(i).lat_e6 / 1e6, s_cams.at(i).lon_e6 / 1e6, rb, sizeof rb);
      snprintf(line, sizeof line, "%s %s", TR("Nearest:"), rb);
      lv_obj_t* card = rowCard(b, [](lv_event_t* e) { cameraSheet((int)(intptr_t)lv_event_get_user_data(e), false); },
                               (void*)(intptr_t)i);
      lbl(card, line, &g_font_14, COLOR_TEXT);
    }
  }
  wideBtn(b, TR("Mark a camera at my position"), [](lv_event_t*) {
    double la, lo;
    if (!gpsFix(&la, &lo)) { toast(TR("No GPS fix: long-press the map instead"), 2400); return; }
    markSheet(la, lo);
  }, nullptr, kMineRgb);
  wideBtn(b, TR("Reload the camera files"), [](lv_event_t*) { camsReload(); pageBuild(); });
  lbl(b, TR("Long-press the map to mark a camera there. Marked cameras are kept in marked.csv next to cameras.csv, "
            "ready to add to OpenStreetMap through deflock.me."), &g_font_12, COLOR_SUB);
}

static void setupToggle(lv_event_t* e) {
  switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case 0: s_cfg.show_cams = !s_cfg.show_cams; s_map_ver++; break;
    case 1: s_cfg.show_hits = !s_cfg.show_hits; s_map_ver++; break;
    case 2: s_cfg.proximity = !s_cfg.proximity; break;
    case 3: s_cfg.radius = (uint8_t)((s_cfg.radius + 1) % flockyou::kRadiusChoices); break;
    case 4:
      if (!s_cfg.share && !s_cfg.channel[0]) { toast(TR("Pick the alert channel first")); return; }
      s_cfg.share = !s_cfg.share;
      break;
    case 5: s_cfg.share_conf = (uint8_t)(s_cfg.share_conf <= flockyou::kConfLow ? flockyou::kConfHigh : s_cfg.share_conf - 1); break;
    case 6: s_cfg.share_marked = !s_cfg.share_marked; break;
    case 7: {   // others' alerts on this channel on the map: Team's watched channels
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
  char line[120];
  auto onoff = [](bool v) { return v ? TR("on") : TR("off"); };
  lbl(b, TR("On the map:"), &g_font_12, COLOR_SUB);
  snprintf(line, sizeof line, "%s: %s", TR("Known cameras"), onoff(s_cfg.show_cams));
  wideBtn(b, line, setupToggle, (void*)0);
  snprintf(line, sizeof line, "%s: %s", TR("What this radio heard"), onoff(s_cfg.show_hits));
  wideBtn(b, line, setupToggle, (void*)1);
  snprintf(line, sizeof line, "%s: %s", TR("Warn near a known camera"), onoff(s_cfg.proximity));
  wideBtn(b, line, setupToggle, (void*)2);
  snprintf(line, sizeof line, "%s: %u m", TR("Warning distance"), flockyou::kRadiusM[s_cfg.radius % flockyou::kRadiusChoices]);
  wideBtn(b, line, setupToggle, (void*)3);

  lbl(b, TR("Alert channel (e.g. #flockwatch; never Public):"), &g_font_12, COLOR_SUB);
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
    lbl(row, TR("No channel to post on. Add one in Chats first: a hashtag channel like #flockwatch, or a private one."),
        &g_font_12, 0xF2A33A);
  snprintf(line, sizeof line, "%s: %s", TR("Post detections there"), onoff(s_cfg.share));
  wideBtn(b, line, setupToggle, (void*)4, s_cfg.share ? 0x2E7D32 : 0);
  static const char* const confs[4] = {"", "everything", "medium and high", "high confidence only"};
  snprintf(line, sizeof line, "%s: %s", TR("Post"), confs[s_cfg.share_conf & 3]);
  wideBtn(b, line, setupToggle, (void*)5);
  snprintf(line, sizeof line, "%s: %s", TR("Post cameras I mark"), onoff(s_cfg.share_marked));
  wideBtn(b, line, setupToggle, (void*)6);
  if (s_cfg.channel[0]) {
    watchfeed::Store* w = teamui::watch();
    snprintf(line, sizeof line, "%s %s %s: %s", TR("Show"), s_cfg.channel, TR("alerts from others on the map"),
             onoff(w && w->isWatched(s_cfg.channel)));
    wideBtn(b, line, setupToggle, (void*)7);
  }
  snprintf(line, sizeof line, "%s %lu", TR("Posts carry your GPS position. At most one a minute, each device once per "
                                          "30 minutes. Posted so far:"), (unsigned long)s_shared_n);
  lbl(b, line, &g_font_12, COLOR_SUB);

  const FlockRadio::Diag& g = FlockRadio::diag();
  char diag[260];
  snprintf(diag, sizeof diag,
           "Wi-Fi frames %lu \xC2\xB7 hits %lu \xC2\xB7 refused %lu \xC2\xB7 yields %lu\n"
           "BLE adverts %lu \xC2\xB7 hits %lu \xC2\xB7 yields %lu \xC2\xB7 queue drops %lu\n"
           "Table %d/%d \xC2\xB7 free PSRAM %lu KB",
           (unsigned long)g.wifi_frames, (unsigned long)g.wifi_hits, (unsigned long)g.wifi_errors, (unsigned long)g.wifi_yield,
           (unsigned long)g.ble_adv, (unsigned long)g.ble_hits, (unsigned long)g.ble_yield, (unsigned long)g.queue_drops,
           s_tab ? s_tab->n : 0, kRows, (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  lbl(b, diag, &g_font_12, 0x7CB342);
  lbl(b, TR("Flock You 2.0 for Wadamesh. Signatures from colonelpanichacks/flock-you and wgreenberg/flock-you; "
            "T-Deck port by Chase H. Barnes. Detections are heuristics: other gear uses the same chips."),
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
  static const char* const names[3] = {"Live", "Cameras", "Setup"};
  for (int i = 0; i < 3; i++) {
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
  else if (s_tab_i == 1) buildCameras(s_body);
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
  if (!ensureTable()) { toast(TR("Out of memory")); return; }
  if (s_root) return;
  if (s_cam_state == CamLoad::Idle) camsReload();
  // Not started by opening the page: scanning runs Wi-Fi and BLE all the time,
  // so it waits for Start.
  s_root = appPageCreateRoot(COLOR_BG);
  appPageBeginSlim("Flock You", pageBack);
  s_tab_i = 0;
  pageBuild();
  // Refresh at most every 2 s, only when something changed; Setup refreshes for its diagnostics.
  s_page_timer = lv_timer_create([](lv_timer_t*) {
    static uint32_t seen = 0, ticks = 0;
    if (s_ver != seen || (s_tab_i == 0 && (++ticks % 3) == 0)) { seen = s_ver; if (s_tab_i != 2) pageBuild(); }
  }, 2000, nullptr);
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

// ---- loop ---------------------------------------------------------------------------------------
static void loopTick() {
  const uint32_t now = millis();
  if (!s_cfg_loaded) {
    loadCfg();
    if (s_cfg.armed) {   // listening survives a reboot
      if (ensureTable()) FlockRadio::setWanted(true);
      else s_cfg.armed = false;
    }
    if (s_cfg_found && (s_cfg.show_cams || s_cfg.proximity)) camsReload();   // never for a radio that never opened it
  }
  camLoadStep();
#if FLOCK_DOWNLOAD
  if (s_cfg_found) dlTick(now);   // a radio that never opened Flock You never downloads
#endif
  FlockRadio::tick(now);
  if (s_tab) {
    flockyou::Alert a;
    int budget = 8;   // the rest waits for the next pass
    while (budget-- > 0 && FlockRadio::pop(a)) hear(a, now);
  }
  proximityTick(now);
  static uint32_t last_share = 0;
  if (now - last_share >= 1000) { last_share = now; shareTick(now); }
  static uint32_t map_seen = 0, last_map = 0;
  if (s_map_ver != map_seen && now - last_map > 1500) {
    map_seen = s_map_ver;
    last_map = now;
    if (s_map_canvas) renderMapMarkers();
  }
}

}  // namespace flockui

#endif  // CAP_FLOCK_YOU
