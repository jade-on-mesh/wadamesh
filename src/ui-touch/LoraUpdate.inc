// SPDX-License-Identifier: GPL-3.0-or-later
// ---- Repeater admin: LoRa Update page -----------------------------------------------
// Included by UITask.cpp after RepeaterRadio2.inc. Opened from the admin console's
// arrow button for the repeater we are logged in to. This standalone serves a
// `.mota` file from the SD card's /mota folder and the repeater pulls it over
// LoRa, both on a temporary radio (Mike Carper's LoRa OTA; LORA_UPDATE.md).
// The steps are his app's (lora_ota_screen.dart), for a repeater in direct range:
//   1. a three-minute safety window: the repeater, then this radio, switch to
//      the temporary radio; the repeater must answer there (`ota status`), or
//      both drop back to normal by themselves when the three minutes end;
//   2. the window is extended to the chosen length and serving starts;
//   3. `ota ls` until the repeater lists our file, then `ota pull <id> flash`;
//   4. `ota status` until "ready to install", then (on a tap) `ota install`;
//   5. this radio goes back to normal (the repeater reboots into the update).
// Stop at any point sends `normalradio` to the repeater and restores this radio.
// The session keeps running if the page or the console is closed.

static fs::FS* backupSdFs();   // UITask.cpp: the SD card, mounted on demand

namespace lup {

enum Step : uint8_t {
  S_IDLE,
  S_TEST_TARGET,     // remote tempradio ...,3 over the normal route
  S_TEST_LOCAL,      // our tempradio ...,3, then a short settle
  S_PROBE,           // remote `ota status` on the temporary radio
  S_EXTEND_TARGET,   // remote tempradio ...,minutes
  S_DISCOVER,        // remote `ota ls` until it lists our file
  S_PULL,            // remote `ota pull <mid> flash`
  S_DOWNLOAD,        // remote `ota status` every so often
  S_READY,           // waiting for the Install tap
  S_INSTALL,         // remote `ota install`
  S_STOP_TARGET,     // remote normalradio (Stop, or a failure after the switch)
  S_DONE,            // finished: this radio is back to normal
};

struct State {
  Step step;
  bool remote_temp;         // the repeater may be on the temporary radio
  bool local_temp;          // we are
  // the command in flight
  bool busy;
  bool temp_route;          // send as a flood on the temporary radio
  char cmd[64];
  char tag[4], old_tag[4];
  uint8_t next_tag, tries;
  uint32_t deadline, timeout_ms;
  uint32_t wait_until;      // the next step starts no earlier than this
  uint8_t attempts;         // discovery / probe rounds
  // the session
  uint8_t file;             // index into loraupdate::file()
  char mid[9];
  float f, bw;
  int sf, cr, minutes;
  loraupdate::DownloadStatus dl;
  uint32_t dl_at;           // millis() of the last status
  char line[96];            // what is happening, for the page
  char log[6][72];          // the last replies, newest last
  uint8_t log_n;
  bool sd_scanned;
  bool user_stop;           // Stop was tapped (not a failure)
};

static State* s = nullptr;
static uint8_t pub[32];
static char name[33];
static lv_obj_t* root = nullptr;
static lv_timer_t* timer = nullptr;
static lv_obj_t *w_files, *w_files_note, *w_freq, *w_bw, *w_sf, *w_cr, *w_min;
static lv_obj_t *w_line, *w_step, *w_bar, *w_dl, *w_sent, *w_log;
static lv_obj_t *w_start, *w_install, *w_stop;
static lv_obj_t* w_file_rows[loraupdate::kMaxFiles];

static constexpr uint32_t kSafetyMinutes = 3;
static constexpr uint32_t kSettleMs = 2500;      // the radio retunes after its OK
static constexpr uint32_t kStatusEveryMs = 15000;

static void render();
static bool sessionRunning();
static void advance(const char* reply);
static void fail(const char* why);

static void logLine(const char* who, const char* t) {
  if (!s) return;
  if (s->log_n == 6) {
    memmove(&s->log[0], &s->log[1], sizeof s->log[0] * 5);
    s->log_n = 5;
  }
  snprintf(s->log[s->log_n++], sizeof s->log[0], "%s: %s", who, t ? t : "");
}

static void setLine(const char* t) {
  if (s) snprintf(s->line, sizeof s->line, "%s", t);
}

static void tempArgs(char* out, size_t cap, int minutes) {
  snprintf(out, cap, "%.3f,%g,%d,%d,%d", s->f, s->bw, s->sf, s->cr, minutes);
}

// ---- talking to the repeater (one tagged command at a time, as rr2) ------------------

static bool sendNow() {
  ContactInfo* c = the_mesh.lookupContactByPubKey(pub, PUB_KEY_SIZE);
  if (!c) return false;
  memcpy(s->old_tag, s->tag, sizeof s->tag);
  snprintf(s->tag, sizeof s->tag, "%02X|", (unsigned)(0xC0 + (s->next_tag++ & 0x3F)));
  char buf[80];
  snprintf(buf, sizeof buf, "%s%s", s->tag, s->cmd);
  int r;
  if (s->temp_route) {
    // On the temporary radio only the repeater listens: flood it there,
    // whatever path the contact keeps for the normal radio.
    ContactInfo flood = *c;
    flood.out_path_len = OUT_PATH_UNKNOWN;
    r = the_mesh.uiSendAdminCommand(flood, buf);
  } else {
    r = the_mesh.uiSendAdminCommand(*c, buf);
  }
  s->deadline = millis() + s->timeout_ms;
  return r == MSG_SEND_SENT_FLOOD || r == MSG_SEND_SENT_DIRECT;
}

static void sendCmd(const char* cmd, bool temp_route) {
  snprintf(s->cmd, sizeof s->cmd, "%s", cmd);
  s->temp_route = temp_route;
  s->tries = 0;
  s->timeout_ms = strncmp(cmd, "ota ", 4) == 0 ? 20000 : 15000;
  s->busy = true;
  if (!sendNow()) {
    s->busy = false;
    fail(TR("Could not send to the repeater"));
  }
}

static bool onAdminReply(const ContactInfo& from, const char* text) {
  if (!s || !text || memcmp(from.id.pub_key, pub, 32) != 0) return false;
  if (strlen(text) < 3 || text[2] != '|') return false;
  if (strncmp(text, s->tag, 3) != 0) return s->old_tag[0] && strncmp(text, s->old_tag, 3) == 0;   // late: drop
  if (!s->busy) return true;
  s->busy = false;
  const char* body = text + 3;
  while (*body == ' ') body++;
  logLine(name, body);
  advance(body);
  render();
  return true;
}

// ---- this radio ---------------------------------------------------------------------

static bool localTemp(int minutes, char* reply, size_t cap) {
  char a[64], cmd[80];
  tempArgs(a, sizeof a, minutes);
  snprintf(cmd, sizeof cmd, "tempradio %s", a);
  if (!mediamesh::runCli(cmd, reply, cap)) snprintf(reply, cap, "ERR tempradio unavailable");
  logLine(TR("This radio"), reply);
  return loraupdate::replyOk(reply);
}

static void localNormal() {
  char r[64];
  if (s->local_temp) {
    mediamesh::runCli("normalradio", r, sizeof r);
    logLine(TR("This radio"), r);
  }
  s->local_temp = false;
}

// ---- the steps -----------------------------------------------------------------------

static void finish(const char* line) {
  loraupdate::stop();
  localNormal();
  s->step = S_DONE;
  s->remote_temp = false;
  setLine(line);
}

// Something went wrong. Before any switch, just say so. After, put the
// repeater back (it would go back by itself when its window ends anyway).
static void fail(const char* why) {
  if (!s) return;
  loraupdate::stop();
  setLine(why);
  if (s->remote_temp && s->step != S_STOP_TARGET) {
    if (!s->local_temp) {   // join it there to tell it
      char r[64];
      s->local_temp = localTemp(kSafetyMinutes, r, sizeof r);
    }
    s->step = S_STOP_TARGET;
    s->wait_until = millis() + kSettleMs;
    return;
  }
  localNormal();
  s->remote_temp = false;
  s->step = s->step == S_STOP_TARGET ? S_DONE : S_IDLE;
}

static void startDiscovery() {
  s->step = S_DISCOVER;
  s->attempts = 0;
  s->wait_until = millis() + 3000;   // let the announcement land first
  setLine(TR("Offering the file to the repeater..."));
}

// A reply to the command in flight: move on.
static void advance(const char* reply) {
  char a[64], cmd[80];
  switch (s->step) {
    case S_TEST_TARGET:
      if (!loraupdate::replyOk(reply)) { fail(TR("The repeater refused the temporary radio")); return; }
      s->remote_temp = true;
      {
        char r[64];
        s->local_temp = localTemp(kSafetyMinutes, r, sizeof r);
        if (!s->local_temp) { fail(TR("This radio could not switch")); return; }
      }
      s->step = S_PROBE;
      s->attempts = 0;
      s->wait_until = millis() + kSettleMs;
      setLine(TR("Checking the repeater answers on the temporary radio..."));
      return;
    case S_PROBE:
      if (loraupdate::replyError(reply)) { fail(TR("The repeater has no LoRa OTA (needs Mike Carper's firmware)")); return; }
      s->step = S_EXTEND_TARGET;
      tempArgs(a, sizeof a, s->minutes);
      snprintf(cmd, sizeof cmd, "tempradio %s", a);
      setLine(TR("Safety check passed. Extending the temporary radio..."));
      sendCmd(cmd, true);
      return;
    case S_EXTEND_TARGET: {
      if (!loraupdate::replyOk(reply)) { fail(TR("The repeater refused to extend the temporary radio")); return; }
      char r[64];
      if (!localTemp(s->minutes, r, sizeof r)) { fail(TR("This radio could not extend")); return; }
      if (!loraupdate::start(s->file)) { fail(TR("Could not open the file (out of memory or card removed)")); return; }
      startDiscovery();
      return;
    }
    case S_DISCOVER:
      if (loraupdate::listsMid(reply, s->mid)) {
        s->step = S_PULL;
        snprintf(cmd, sizeof cmd, "ota pull %s flash", s->mid);
        setLine(TR("Starting the download..."));
        sendCmd(cmd, true);
        return;
      }
      if (++s->attempts >= 6) { fail(TR("The repeater never listed the file. Is it made for this repeater's board?")); return; }
      s->wait_until = millis() + 5000;
      return;
    case S_PULL:
      if (loraupdate::replyError(reply)) {
        char m[96];
        snprintf(m, sizeof m, "%s %s", TR("Refused:"), reply);
        fail(m);
        return;
      }
      s->step = S_DOWNLOAD;
      s->dl = loraupdate::DownloadStatus{};
      s->wait_until = millis() + 5000;
      setLine(TR("Downloading..."));
      return;
    case S_DOWNLOAD: {
      const loraupdate::DownloadStatus d = loraupdate::parseStatus(reply);
      s->dl_at = millis();
      if (d.known) s->dl = d;
      if (d.mid[0] && strcmp(d.mid, s->mid) != 0) { fail(TR("The repeater is downloading a different file")); return; }
      if (d.failed) { fail(TR("The download failed")); return; }
      if (d.ready) {
        s->dl = d;
        s->step = S_READY;
        setLine(TR("Downloaded and checked. Ready to install."));
        return;
      }
      s->wait_until = millis() + kStatusEveryMs;
      return;
    }
    case S_INSTALL:
      if (loraupdate::replyError(reply)) {
        char m[96];
        snprintf(m, sizeof m, "%s %s", TR("Refused:"), reply);
        fail(m);
        return;
      }
      finish(TR("Install started. The repeater reboots into the new firmware; check its version in a minute."));
      return;
    case S_STOP_TARGET:
      s->remote_temp = false;
      localNormal();
      s->step = S_DONE;
      if (s->user_stop) setLine(TR("Stopped. Both radios are back to normal."));
      return;
    default:
      return;
  }
}

// No reply in time.
static void timedOut() {
  if (s->tries++ < 1 && sendNow()) return;   // one more try
  s->busy = false;
  switch (s->step) {
    case S_TEST_TARGET:
      // It may still have switched: treat it as on the temporary radio.
      s->remote_temp = true;
      fail(TR("No answer from the repeater"));
      return;
    case S_PROBE:
      if (++s->attempts < 2) { s->wait_until = millis() + 2000; return; }
      fail(TR("The repeater did not answer on the temporary radio. Both go back to normal within 3 minutes."));
      return;
    case S_DISCOVER:
      if (++s->attempts >= 6) { fail(TR("The repeater stopped answering")); return; }
      s->wait_until = millis() + 5000;
      return;
    case S_DOWNLOAD:
      s->wait_until = millis() + 5000;   // keep asking: Stop ends it, and so does the window
      return;
    case S_INSTALL:
      finish(TR("No answer to install: it may already be rebooting. Check its version before trying again."));
      return;
    case S_STOP_TARGET:
      localNormal();
      s->remote_temp = false;
      s->step = S_DONE;
      setLine(TR("The repeater did not answer. It goes back to normal when its temporary radio ends."));
      return;
    default:
      fail(TR("No answer from the repeater"));
      return;
  }
}

// Start the next command once its moment comes.
static void kick() {
  if (s->busy || (int32_t)(millis() - s->wait_until) < 0) return;
  switch (s->step) {
    case S_PROBE:     sendCmd("ota status", true); break;
    case S_DISCOVER:
      if (s->attempts == 2) loraupdate::announce();   // in case the first was missed
      sendCmd("ota ls", true);
      break;
    case S_DOWNLOAD:  sendCmd("ota status", true); break;
    case S_STOP_TARGET:
      sendCmd("normalradio", true);
      break;
    default: break;
  }
}

static void tick(lv_timer_t*) {
  if (!s) return;
  if (!root && !sessionRunning()) {   // finished while the page was closed
    if (timer) { lv_timer_del(timer); timer = nullptr; }
    return;
  }
  if (s->busy && (int32_t)(millis() - s->deadline) >= 0) timedOut();
  if (!s->busy) kick();
  // This radio's own window ending (it was too short) ends the session.
  if (s->local_temp && !mediamesh::status().temp1) {
    s->local_temp = false;
    if (s->step != S_DONE && s->step != S_IDLE) {
      loraupdate::stop();
      s->remote_temp = false;
      s->busy = false;
      s->step = S_DONE;
      setLine(TR("The temporary radio ended. Start again with a longer window."));
    }
  }
  render();
}

static bool sessionRunning() { return s && s->step != S_IDLE && s->step != S_DONE; }

// ---- buttons ---------------------------------------------------------------------------

static void startNow() {
  if (!s || sessionRunning()) return;
  kbMirrorSyncToReal();
  float f = 0, bw = 0;
  int sf = 0, cr = 0, mins = 0;
  if (!parseFloatField(w_freq, f) || !parseFloatField(w_bw, bw) || !parseIntField(w_sf, sf) ||
      !parseIntField(w_cr, cr) || !parseIntField(w_min, mins)) {
    if (g_lv.task) g_lv.task->showAlert(TR("Fill in every temporary radio field"), 1800);
    return;
  }
  if (f < 150 || f > 2500 || sf < 5 || sf > 12 || cr < 5 || cr > 8 || mins < 10 || mins > 10080) {
    if (g_lv.task) g_lv.task->showAlert(TR("Check the values: SF 5-12, CR 5-8, 10 minutes or more"), 2200);
    return;
  }
  const loraupdate::FileInfo* fi = loraupdate::file(s->file);
  if (!fi) {
    if (g_lv.task) g_lv.task->showAlert(TR("Choose a firmware file first"), 1600);
    return;
  }
  s->f = f; s->bw = bw; s->sf = sf; s->cr = cr; s->minutes = mins;
  loraupdate::midHex(fi->desc.mid, s->mid);
  s->log_n = 0;
  s->remote_temp = s->local_temp = s->user_stop = false;
  s->dl = loraupdate::DownloadStatus{};
  char a[64], cmd[80];
  tempArgs(a, sizeof a, kSafetyMinutes);
  snprintf(cmd, sizeof cmd, "tempradio %s", a);
  s->step = S_TEST_TARGET;
  setLine(TR("Starting a three-minute safety window on the repeater..."));
  sendCmd(cmd, false);
  render();
}

static void startCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s) return;
  hideKb();
  showConfirm(TR("Start the update? The repeater and this radio move to the temporary radio and are off the normal mesh until it ends."),
              TR("Start"), startNow);
}

static void installNow() {
  if (!s || s->step != S_READY) return;
  s->step = S_INSTALL;
  setLine(TR("Installing..."));
  sendCmd("ota install", true);
  render();
}

static void installCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s || s->step != S_READY) return;
  showConfirm(TR("Install now? The repeater flashes the new firmware and reboots. Keep it powered."),
              TR("Install"), installNow);
}

static void stopNow() {
  if (!s || !sessionRunning()) return;
  s->busy = false;
  s->user_stop = true;
  loraupdate::stop();
  if (s->remote_temp) {
    s->step = S_STOP_TARGET;
    s->wait_until = millis();
    setLine(TR("Putting the repeater back on its normal radio..."));
  } else {
    localNormal();
    s->step = S_IDLE;
    setLine(TR("Stopped"));
  }
  render();
}

static void stopCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s) return;
  showConfirm(TR("Stop the update and put both radios back to normal?"), TR("Stop"), stopNow);
}

static void fileCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED || !s || sessionRunning()) return;
  s->file = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  render();
}

static void rescan() {
  if (!s || sessionRunning()) return;
  loraupdate::scan(backupSdFs());
  s->sd_scanned = true;
  if (s->file >= loraupdate::fileCount()) s->file = 0;
}

static void refreshCb(lv_event_t* e);

// ---- the page ---------------------------------------------------------------------------

static const char* stepName(Step st) {
  switch (st) {
    case S_IDLE: return TR("Ready");
    case S_TEST_TARGET: case S_TEST_LOCAL: case S_PROBE: return TR("Safety check");
    case S_EXTEND_TARGET: return TR("Switching");
    case S_DISCOVER: return TR("Offering");
    case S_PULL: case S_DOWNLOAD: return TR("Downloading");
    case S_READY: return TR("Ready to install");
    case S_INSTALL: return TR("Installing");
    case S_STOP_TARGET: return TR("Stopping");
    case S_DONE: return TR("Finished");
  }
  return "";
}

static void showIf(lv_obj_t* o, bool on) {
  if (!o) return;
  if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void render() {
  if (!s || !root) return;
  const bool running = sessionRunning();
  const uint8_t n = loraupdate::fileCount();
  for (uint8_t i = 0; i < loraupdate::kMaxFiles; i++) {
    if (!w_file_rows[i]) continue;
    showIf(w_file_rows[i], i < n);
    if (i < n) setActionValue(w_file_rows[i], i == s->file ? TR("Selected") : "");
  }
  if (w_files_note) {
    char b[160];
    if (!n) snprintf(b, sizeof b, "%s", TR("No firmware here. Put .mota files in the /mota folder on the SD card, then tap Refresh."));
    else if (loraupdate::skipped()) snprintf(b, sizeof b, "%u %s", (unsigned)loraupdate::skipped(), TR("file(s) in /mota are not valid .mota files and are left out."));
    else b[0] = 0;
    lv_label_set_text(w_files_note, b);
  }
  if (w_line) lv_label_set_text(w_line, s->line[0] ? s->line : TR("Choose a file and start."));
  if (w_step) lv_label_set_text(w_step, stepName(s->step));
  if (w_bar) {
    const int pc = s->step == S_READY || s->step == S_INSTALL ? 100 : (s->dl.percent < 0 ? 0 : s->dl.percent);
    lv_bar_set_value(w_bar, pc, LV_ANIM_OFF);
  }
  if (w_dl) {
    char b[48];
    if (s->dl.known) snprintf(b, sizeof b, "%lu/%lu (%d%%)", (unsigned long)s->dl.have, (unsigned long)s->dl.total, s->dl.percent < 0 ? 0 : s->dl.percent);
    else snprintf(b, sizeof b, "-");
    lv_label_set_text(w_dl, b);
  }
  if (w_sent) {
    char b[24];
    snprintf(b, sizeof b, "%lu", (unsigned long)loraupdate::packetsSent());
    lv_label_set_text(w_sent, b);
  }
  if (w_log) {
    char b[6 * 74] = "";
    size_t k = 0;
    for (uint8_t i = 0; i < s->log_n && k < sizeof b; i++) k += snprintf(b + k, sizeof b - k, "%s%s", i ? "\n" : "", s->log[i]);
    lv_label_set_text(w_log, b[0] ? b : "-");
  }
  showIf(w_start, !running && n > 0);
  showIf(w_install, s->step == S_READY);
  showIf(w_stop, running && s->step != S_STOP_TARGET);
}

void openPage(const uint8_t* pub32, const char* nm);

// Re-read the card and rebuild the page (the file rows), keeping what was typed.
static void refreshNow() {
  if (!s || sessionRunning()) return;
  kbMirrorSyncToReal();
  float f = 0, bw = 0;
  int v = 0;
  if (w_freq && parseFloatField(w_freq, f)) s->f = f;
  if (w_bw && parseFloatField(w_bw, bw)) s->bw = bw;
  if (w_sf && parseIntField(w_sf, v)) s->sf = v;
  if (w_cr && parseIntField(w_cr, v)) s->cr = v;
  if (w_min && parseIntField(w_min, v)) s->minutes = v;
  s->line[0] = 0;
  uint8_t key[32];
  char nm[33];
  memcpy(key, pub, 32);
  snprintf(nm, sizeof nm, "%s", name);
  openPage(key, nm);   // rescans, as the page opens
}

static void refreshCb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  lv_async_call([](void*) { refreshNow(); }, nullptr);   // the button is deleted with the page
}

void closePage() {
  if (root) { hideKb(); popupClose(&root); }
  root = nullptr;
  w_files = w_files_note = w_freq = w_bw = w_sf = w_cr = w_min = nullptr;
  w_line = w_step = w_bar = w_dl = w_sent = w_log = nullptr;
  w_start = w_install = w_stop = nullptr;
  memset(w_file_rows, 0, sizeof w_file_rows);
  // A running session keeps its timer (and its radios); an idle one is let go.
  if (!sessionRunning()) {
    if (timer) { lv_timer_del(timer); timer = nullptr; }
  }
}

static void build(lv_obj_t* page) {
  s_settings_inline_parent = page;
  char b[96];
  {
    lv_obj_t* c = setCard("Firmware file");
    for (uint8_t i = 0; i < loraupdate::kMaxFiles; i++) {
      const loraupdate::FileInfo* fi = loraupdate::file(i);
      if (!fi) { w_file_rows[i] = nullptr; continue; }
      char ver[20], mid[9];
      loraupdate::versionText(fi->desc.fw_version, ver, sizeof ver);
      loraupdate::midHex(fi->desc.mid, mid);
      snprintf(b, sizeof b, "%s  %s  %s", fi->name, ver, mid);
      w_file_rows[i] = setActionRowRaw(c, UI_ICON_FILE_TEXT, b, "", fileCb, (void*)(intptr_t)i, SET_NORMAL, false);
    }
    w_files_note = setNote(c, "");
    setActionRow(c, UI_ICON_REFRESH_CW, "Refresh", "", refreshCb, nullptr, SET_NORMAL, false);
    setNote(c, "Build .mota files with Mike Carper's motatool, or download them in MeshCore Open TAK. Use a file made for this repeater's board.");
  }
  {
    lv_obj_t* c = setCard("Temporary radio");
    setNote(c, "The repeater and this radio meet here for the transfer, away from the normal mesh. Pick a quiet frequency you may use. The repeater must be in direct range.");
    lv_obj_t* r1 = setFieldRow(c);
    w_freq = setField(r1, "Frequency (MHz)", "MHz", 10, setFieldW(2));
    w_bw = setField(r1, "Bandwidth (kHz)", "kHz", 7, setFieldW(2));
    lv_obj_t* r2 = setFieldRow(c);
    w_sf = setField(r2, "SF", "SF", 2, setFieldW(3));
    w_cr = setField(r2, "CR", "CR", 1, setFieldW(3));
    w_min = setField(r2, "Minutes", "min", 5, setFieldW(3));
    snprintf(b, sizeof b, "%.3f", s->f); lv_textarea_set_text(w_freq, b);
    snprintf(b, sizeof b, "%g", s->bw); lv_textarea_set_text(w_bw, b);
    snprintf(b, sizeof b, "%d", s->sf); lv_textarea_set_text(w_sf, b);
    snprintf(b, sizeof b, "%d", s->cr); lv_textarea_set_text(w_cr, b);
    snprintf(b, sizeof b, "%d", s->minutes); lv_textarea_set_text(w_min, b);
  }
  {
    lv_obj_t* c = setCard("Update");
    w_line = setNote(c, "");
    w_step = setInfoRow(c, "Step", "-");
    w_bar = lv_bar_create(c);
    lv_obj_set_size(w_bar, s_settings_content_w, SC(8));
    lv_bar_set_range(w_bar, 0, 100);
    lv_obj_set_style_bg_color(w_bar, lv_color_hex(COLOR_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(w_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(w_bar, lv_color_hex(COLOR_STATUS_OK_TEXT), LV_PART_INDICATOR);
    w_dl = setInfoRow(c, "Blocks", "-");
    w_sent = setInfoRow(c, "Packets sent", "0");
    w_start = setButton(c, UI_ICON_ARROW_UP, "Start update", startCb, nullptr, SET_PRIMARY);
    w_install = setButton(c, UI_ICON_CHECK, "Install and reboot", installCb, nullptr, SET_PRIMARY);
    w_stop = setButton(c, UI_ICON_X, "Stop", stopCb, nullptr, SET_DANGER);
  }
  {
    lv_obj_t* c = setCard("Log");
    w_log = lv_label_create(c);
    lv_obj_set_width(w_log, s_settings_content_w);
    lv_label_set_long_mode(w_log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(w_log, &g_font_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(w_log, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
  }
  s_settings_inline_parent = nullptr;
}

void openPage(const uint8_t* pub32, const char* nm) {
  if (sessionRunning() && memcmp(pub, pub32, 32) != 0) {
    char m[80];
    snprintf(m, sizeof m, "%s %s", TR("A LoRa Update is running for"), name);
    if (g_lv.task) g_lv.task->showAlert(m, 2200);
    return;
  }
  closePage();
  if (!s) {
    s = (State*)psAlloc(sizeof(State));
    if (!s) { if (g_lv.task) g_lv.task->showAlert(TR("Out of memory"), 1500); return; }
    *s = State{};
  }
  if (!sessionRunning()) {
    const bool same = memcmp(pub, pub32, 32) == 0;
    const State keep = *s;
    *s = State{};
    // Mike Carper's app defaults; the last values typed stay for the next time.
    s->f = same && keep.f > 0 ? keep.f : 909.950f;
    s->bw = same && keep.bw > 0 ? keep.bw : 250;
    s->sf = same && keep.sf ? keep.sf : 5;
    s->cr = same && keep.cr ? keep.cr : 5;
    s->minutes = same && keep.minutes ? keep.minutes : 120;
    memcpy(pub, pub32, 32);
    snprintf(name, sizeof name, "%s", nm ? nm : "");
    rescan();
  }

  const lv_coord_t sw = lv_disp_get_hor_res(nullptr), sh = lv_disp_get_ver_res(nullptr);
  root = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, sw, sh - STATUSBAR_H);
  lv_obj_set_pos(root, 0, STATUSBAR_H);
  styleSurface(root, COLOR_BG, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* hdr = lv_obj_create(root);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_size(hdr, sw, 36);
  styleSurface(hdr, COLOR_PANEL, 0);
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(hdr, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(hdr, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* title = lv_label_create(hdr);
  char tb[56];
  snprintf(tb, sizeof tb, "%s: %s", TR("LoRa Update"), name);
  lv_label_set_text(title, tb);
  lv_obj_set_style_text_font(title, &g_font_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(title, sw - 8 - 36);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
  addCloseXBadge(hdr, +[](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) closePage();
  });

  lv_obj_t* page = lv_obj_create(root);
  lv_obj_set_pos(page, 0, 36);
  lv_obj_set_size(page, sw, sh - STATUSBAR_H - 36);
  prepSettingsPage(page);
  build(page);
  if (!timer) timer = lv_timer_create(tick, 500, nullptr);
  if (!mediamesh::supported()) {
    setLine(TR("This radio can't run a temporary radio (needs an SX1262), so it can't serve updates."));
    if (w_start) lv_obj_add_state(w_start, LV_STATE_DISABLED);
  }
  render();
}

}  // namespace lup
