// SPDX-License-Identifier: GPL-3.0-or-later
// OTP Messenger: the app's screens. Included by UITask.cpp (it draws with the
// firmware's own widgets, fonts, keyboard and dialogs); the protocol lives in
// OtpCore.cpp and the mesh/storage glue in OtpMessenger.cpp.
//
// Screens: Lock -> Conversations -> Chat -> Pad, plus New conversation and
// Settings. Back (the bar, Esc) goes one screen up and closes from the list.

#include "OtpMessenger.h"

namespace otpui {

enum class Screen : uint8_t { List, Pick, Chat, Pad, Settings, Lock, Import };

// The six themes of OTP_3_RC1.lua, same names and colours.
struct Theme { const char* name; uint32_t encrypt, decrypt, ciphertext, plaintext, newpad, channel, ok, keyphrase, padinfo, ready; };
static const Theme kThemes[6] = {
  {"Default",   0xFF6B4A, 0x2DD4BF, 0xF97316, 0x38BDF8, 0xA78BFA, 0xEC4899, 0x4ADE80, 0xFBBF24, 0x22D3EE, 0x94A3B8},
  {"Cyberpunk", 0xFF1493, 0x39FF14, 0xBC13FE, 0x00F5FF, 0xFF00FF, 0xF5FF00, 0x39FF14, 0xFFAA00, 0x00FFEF, 0x6B7280},
  {"Trans",     0xF5A9B8, 0x5BCEFA, 0xF5A9B8, 0xFFFFFF, 0xA6E8FF, 0xFF69B4, 0x5BCEFA, 0xF5A9B8, 0x38BDF8, 0xE5E7EB},
  {"Cascadia",  0x60A5FA, 0x38BDF8, 0xA78BFA, 0xFFFFFF, 0xFBBF24, 0x15803D, 0x84CC16, 0xF472B6, 0x22C55E, 0xCBD5E1},
  {"B&W",       0xFFFFFF, 0xE6E6E6, 0xCCCCCC, 0xFFFFFF, 0xB3B3B3, 0xD9D9D9, 0xFFFFFF, 0x999999, 0x808080, 0x4D4D4D},
  {"Acid",      0xFF4500, 0xFFA500, 0x2F4F4F, 0xFFFFFF, 0x4682B4, 0xFF0000, 0xFFD700, 0xFFDAB9, 0x8B0000, 0x696969},
};

static lv_obj_t* s_root = nullptr;    // page root
static lv_obj_t* s_body = nullptr;    // current screen
static Screen s_screen = Screen::List;
static char s_tid[48] = {0};          // open conversation
static uint32_t s_seen = 0;
static lv_timer_t* s_timer = nullptr;
static bool s_pick_channels = false;
// chat widgets
static lv_obj_t* s_list = nullptr;     // bubble column
static lv_obj_t* s_ta = nullptr;
static lv_obj_t* s_counter = nullptr;
static lv_obj_t* s_info = nullptr;
static lv_obj_t* s_parts = nullptr;
static lv_obj_t* s_bar = nullptr;
static lv_obj_t* s_send_btn = nullptr;
static int s_shown_n = 60;             // bubbles drawn (newest)
// prompt
static lv_obj_t* s_prompt = nullptr;
static lv_obj_t* s_prompt_ta = nullptr;
static void (*s_prompt_cb)(const char*) = nullptr;
static uint32_t s_wipe_armed = 0;

static const Theme& th() { return kThemes[OtpMessenger::core().theme % 6]; }
static otp::Target* cur() { return s_tid[0] ? OtpMessenger::core().findById(s_tid) : nullptr; }
static void toast(const char* m, int ms = 1800) { if (g_lv.task) g_lv.task->showAlert(m, ms); }

static void show(Screen s);
static void close();
static void back();

// ---- colour helpers (contrast_of / darken / lighten from the Lua app) ----------
static uint32_t darken(uint32_t c, float f) {
  return ((uint32_t)(((c >> 16) & 0xFF) * f) << 16) | ((uint32_t)(((c >> 8) & 0xFF) * f) << 8) | (uint32_t)((c & 0xFF) * f);
}
static uint32_t lighten(uint32_t c, float t) {
  uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  r = (uint32_t)(r + (255 - r) * t); g = (uint32_t)(g + (255 - g) * t); b = (uint32_t)(b + (255 - b) * t);
  return (r << 16) | (g << 8) | b;
}
static float lin(float c) { c /= 255.0f; return c <= 0.03928f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float lum(uint32_t h) { return 0.2126f * lin((h >> 16) & 0xFF) + 0.7152f * lin((h >> 8) & 0xFF) + 0.0722f * lin(h & 0xFF); }
static uint32_t contrastOf(uint32_t col) {
  const float l = lum(col) + 0.05f;
  if (0.96f / l >= 4.5f) return 0xF5F5F5;
  const uint32_t shade = darken(col, 0.2f);
  if (l / (lum(shade) + 0.05f) >= 4.5f) return shade;
  return 0x14171C;
}
static uint32_t senderColor(const char* who) {
  // Stable per name (the Lua app assigned buckets in arrival order).
  uint32_t h = 2166136261u;
  for (const char* c = who; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
  const Theme& t = th();
  const uint32_t opts[5] = {t.decrypt, t.ciphertext, t.newpad, t.keyphrase, t.ok};
  return opts[h % 5];
}

// ---- small widget helpers --------------------------------------------------------
static lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  return l;
}

static lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* ud = nullptr,
                        uint32_t bg = 0, lv_coord_t h = 30) {
  lv_obj_t* b = lv_btn_create(parent);
  styleButton(b);
  lv_obj_set_height(b, h);
  lv_obj_set_style_pad_hor(b, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(b, 0, LV_PART_MAIN);
  if (bg) {
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_text_color(b, lv_color_hex(contrastOf(bg)), LV_PART_MAIN);
  }
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &g_font_14, LV_PART_MAIN);
  useChainedFont(l);
  lv_obj_center(l);
  return b;
}

static lv_obj_t* column(lv_obj_t* parent, lv_coord_t pad = 6, lv_coord_t gap = 6) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(c, pad, LV_PART_MAIN);
  lv_obj_set_style_pad_row(c, gap, LV_PART_MAIN);
  return c;
}

static lv_obj_t* row(lv_obj_t* parent, lv_coord_t gap = 6) {
  lv_obj_t* r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, gap, LV_PART_MAIN);
  lv_obj_set_style_pad_row(r, gap, LV_PART_MAIN);
  return r;
}

// A scrolling screen body below the bar.
static lv_obj_t* newBody(bool scroll = true) {
  // Screens are swapped from their own buttons' handlers: never delete the
  // tree an event is still being dispatched in.
  if (s_body) { lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN); lv_obj_del_async(s_body); s_body = nullptr; }
  s_list = s_ta = s_counter = s_info = s_parts = s_bar = s_send_btn = nullptr;
  s_body = lv_obj_create(s_root);
  lv_obj_remove_style_all(s_body);
  lv_obj_set_size(s_body, LV_PCT(100), LV_PCT(100));
  if (scroll) {
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_body, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_body, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
  } else {
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);
  }
  return s_body;
}

static lv_obj_t* heading(lv_obj_t* parent, const char* text) {
  lv_obj_t* l = label(parent, text, &g_font_16, th().channel);
  useChainedFont(l);
  return l;
}

static lv_obj_t* para(lv_obj_t* parent, const char* text, uint32_t color = 0) {
  lv_obj_t* l = label(parent, text, &g_font_12, color ? color : COLOR_SUB);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  useChainedFont(l);
  return l;
}

static void fmtAgo(uint32_t epoch, uint32_t at_ms, char* out, size_t cap) {
  const uint32_t now_e = the_mesh.getRTCClock()->getCurrentTime();
  if (epoch && now_e > 1700000000UL) {
    time_t t = (time_t)epoch, n = (time_t)now_e;
    struct tm a, b;
    if (localtime_r(&t, &a) && localtime_r(&n, &b)) {
      if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) strftime(out, cap, "%H:%M", &a);
      else strftime(out, cap, "%b %d, %H:%M", &a);
      return;
    }
  }
  if (!at_ms) { out[0] = 0; return; }
  const uint32_t s = (millis() - at_ms) / 1000;
  if (s < 5) snprintf(out, cap, "just now");
  else if (s < 60) snprintf(out, cap, "%us ago", (unsigned)s);
  else if (s < 3600) snprintf(out, cap, "%um ago", (unsigned)(s / 60));
  else if (s < 86400) snprintf(out, cap, "%uh ago", (unsigned)(s / 3600));
  else snprintf(out, cap, "%ud ago", (unsigned)(s / 86400));
}

// ---- text prompt (long fields: pad hex, passphrases) --------------------------------
static void promptClose() {
  if (g_lv.keyboard) lv_keyboard_set_textarea(g_lv.keyboard, nullptr);
  if (s_prompt) popupClose(&s_prompt);
  s_prompt_ta = nullptr;
}
static void promptDone(bool ok) {
  void (*cb)(const char*) = s_prompt_cb;
  s_prompt_cb = nullptr;
  char* text = nullptr;
  if (ok && s_prompt_ta) {
    kbMirrorSyncToReal();
    const char* t = lv_textarea_get_text(s_prompt_ta);
    text = strdup(t ? t : "");
  }
  promptClose();
  hideKb();
  if (cb) cb(ok ? (text ? text : "") : nullptr);
  if (text) { memset(text, 0, strlen(text)); free(text); }
}
static void prompt(const char* title, const char* hint, bool secret, uint32_t max_len, void (*cb)(const char*)) {
  promptClose();
  s_prompt_cb = cb;
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr), sh = lv_disp_get_ver_res(nullptr);
  s_prompt = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_prompt);
  lv_obj_set_size(s_prompt, sw, sh - STATUSBAR_H);
  lv_obj_set_pos(s_prompt, 0, STATUSBAR_H);
  lv_obj_set_style_bg_color(s_prompt, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_prompt, LV_OPA_60, LV_PART_MAIN);
  lv_obj_add_flag(s_prompt, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_prompt, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* card = lv_obj_create(s_prompt);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, sw - 20, LV_SIZE_CONTENT);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 6);
  styleCard(card);
  lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
  lv_obj_t* tl = label(card, title, &g_font_14, COLOR_TEXT);
  useChainedFont(tl);
  if (hint && *hint) para(card, hint);
  s_prompt_ta = lv_textarea_create(card);
  lv_obj_set_width(s_prompt_ta, LV_PCT(100));
  styleCard(s_prompt_ta);
  lv_obj_set_style_text_font(s_prompt_ta, &g_font_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_prompt_ta, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  lv_textarea_set_max_length(s_prompt_ta, max_len);
  if (secret) {
    lv_textarea_set_one_line(s_prompt_ta, true);
    lv_textarea_set_password_mode(s_prompt_ta, true);
  } else {
    lv_obj_set_height(s_prompt_ta, 64);
  }
  attachSettingsTaEvents(s_prompt_ta);
  lv_obj_add_event_cb(s_prompt_ta, [](lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_READY) promptDone(true);
  }, LV_EVENT_READY, nullptr);
  lv_obj_t* r = row(card);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  button(r, TR("Cancel"), [](lv_event_t*) { promptDone(false); }, nullptr, COLOR_SECONDARY_ACTION);
  button(r, "OK", [](lv_event_t*) { promptDone(true); }, nullptr, COLOR_STATUS_OK);
  if (g_lv.keyboard) kbMirrorBind(s_prompt_ta);
  lv_group_t* g = lv_group_get_default();
  if (g) lv_group_focus_obj(s_prompt_ta);
}

// ---- Lock screen ---------------------------------------------------------------------
static void unlockWith(const char* pass) {
  if (!pass) return;
  if (!OtpMessenger::core().unlock(pass)) { toast(TR("Wrong passphrase"), 1600); return; }
  show(Screen::List);
}
static void panicConfirmed() {
  OtpMessenger::core().panicWipe();
  toast(TR("Everything wiped. Protection is off."), 2200);
  s_tid[0] = 0;
  show(Screen::List);
}
static void buildLock() {
  lv_obj_t* b = newBody();
  heading(b, "OTP Messenger");
  para(b, TR("Locked. Your pads and messages are encrypted with your passphrase. "
             "Messages that arrive while locked are kept and opened when you unlock."));
  button(b, TR("Enter passphrase"), [](lv_event_t*) {
    prompt(TR("Passphrase"), nullptr, true, 64, unlockWith);
  }, nullptr, th().channel);
  button(b, TR("Panic wipe"), [](lv_event_t*) {
    showConfirm(TR("Erase every pad and message on this device, and turn protection off?\n\nThis cannot be undone."),
                TR("Wipe"), panicConfirmed);
  }, nullptr, COLOR_STATUS_DANGER);
}

// ---- Conversations ---------------------------------------------------------------------
static void openChatCb(lv_event_t* e) {
  otp::Target* t = (otp::Target*)lv_event_get_user_data(e);
  if (!t) return;
  snprintf(s_tid, sizeof s_tid, "%s", t->id);
  show(Screen::Chat);
}

static const otp::Entry* lastShown(otp::Target& t) {
  OtpMessenger::core().ensureLog(t);
  return t.log_n ? &t.log[t.log_n - 1] : nullptr;
}

static void buildList() {
  otp::Core& c = OtpMessenger::core();
  lv_obj_t* b = newBody();
  lv_obj_t* top = row(b);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_t* h = heading(top, TR("Conversations"));
  lv_obj_set_flex_grow(h, 1);
  button(top, LV_SYMBOL_PLUS, [](lv_event_t*) { show(Screen::Pick); }, nullptr, th().channel, 28);
  button(top, LV_SYMBOL_SETTINGS, [](lv_event_t*) { show(Screen::Settings); }, nullptr, 0, 28);

  // Newest activity first.
  otp::Target* order[otp::kMaxTargets];
  int n = 0;
  for (int i = 0; i < c.targetCount(); i++) {
    otp::Target* t = c.targetAt(i);
    if (!t || !t->keep) continue;
    order[n++] = t;
  }
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0; j--) {
      const otp::Entry* a = lastShown(*order[j - 1]);
      const otp::Entry* z = lastShown(*order[j]);
      const uint32_t ka = a ? a->seq : 0, kz = z ? z->seq : 0;
      if (kz > ka) { otp::Target* x = order[j]; order[j] = order[j - 1]; order[j - 1] = x; }
    }
  if (!n) {
    para(b, TR("No conversations yet.\n\nTap + to pick a contact or a private channel, then give it a pad: "
               "generate one and export it to the SD card for the other device, or import theirs."));
    return;
  }
  for (int i = 0; i < n; i++) {
    otp::Target* t = order[i];
    lv_obj_t* card = lv_obj_create(b);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    styleCard(card);
    lv_obj_set_style_pad_all(card, 6, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, openChatCb, LV_EVENT_CLICKED, t);
    char title[64];
    snprintf(title, sizeof title, "%s%s", t->kind == otp::Kind::Dm ? "" : "#", t->name);
    lv_obj_t* tl = label(card, title, &g_font_14, t->kind == otp::Kind::Dm ? senderColor(t->name) : th().channel);
    useChainedFont(tl);
    lv_obj_set_width(tl, LV_PCT(70));
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    // pad bytes, right
    char right[48];
    const int avail = c.available(*t);
    if (t->unread) snprintf(right, sizeof right, "%u new", (unsigned)t->unread);
    else if (avail > 0) snprintf(right, sizeof right, "%d B", avail);
    else snprintf(right, sizeof right, TR("no pad"));
    lv_obj_t* rl = label(card, right, &g_font_12, t->unread ? th().ok : (avail < otp::kLowPadBytes ? th().encrypt : th().padinfo));
    lv_obj_align(rl, LV_ALIGN_TOP_RIGHT, 0, 2);
    const otp::Entry* e = lastShown(*t);
    char prev[96];
    if (e) snprintf(prev, sizeof prev, "%s%.*s", e->mine ? TR("You: ") : "", (int)(e->len > 70 ? 70 : e->len), e->text);
    else snprintf(prev, sizeof prev, "%s", TR("No messages yet"));
    for (char* p = prev; *p; ++p) if (*p == '\n') *p = ' ';
    lv_obj_t* pl = label(card, prev, &g_font_12, COLOR_SUB);
    useChainedFont(pl);
    lv_obj_set_width(pl, LV_PCT(100));
    lv_label_set_long_mode(pl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(pl, 0, 20);
  }
}

// ---- New conversation ----------------------------------------------------------------------
static void pickContactCb(lv_event_t* e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  uint8_t pub[32];
  char name[33];
  if (!OtpMessenger::contactAt(i, pub, name, sizeof name)) return;
  otp::Target* t = OtpMessenger::core().dmTarget(pub, name);
  if (!t) { toast(TR("Too many conversations")); return; }
  snprintf(s_tid, sizeof s_tid, "%s", t->id);
  show(OtpMessenger::core().hasPad(*t) ? Screen::Chat : Screen::Pad);
}
static void pickChannelCb(lv_event_t* e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  char name[33];
  if (!OtpMessenger::channelAt(i, name, sizeof name)) return;
  otp::Target* t = OtpMessenger::core().channelTarget(name);
  if (!t) { toast(TR("OTP is not available on the Public channel"), 2200); return; }
  snprintf(s_tid, sizeof s_tid, "%s", t->id);
  show(OtpMessenger::core().hasPad(*t) ? Screen::Chat : Screen::Pad);
}
static void buildPick() {
  lv_obj_t* b = newBody();
  heading(b, TR("New conversation"));
  lv_obj_t* seg = row(b);
  button(seg, TR("Contacts"), [](lv_event_t*) { s_pick_channels = false; show(Screen::Pick); }, nullptr,
         s_pick_channels ? 0 : th().channel, 28);
  button(seg, TR("Channels"), [](lv_event_t*) { s_pick_channels = true; show(Screen::Pick); }, nullptr,
         s_pick_channels ? th().channel : 0, 28);
  otp::Core& c = OtpMessenger::core();
  int shown = 0;
  if (!s_pick_channels) {
    const int n = OtpMessenger::contactCount();
    for (int i = 0; i < n && shown < 120; i++) {
      uint8_t pub[32];
      char name[33];
      if (!OtpMessenger::contactAt(i, pub, name, sizeof name)) continue;
      char hex[9]; otp::toHex(pub, 4, hex);
      char id[16]; snprintf(id, sizeof id, "dm:%s", hex);
      otp::Target* t = c.findById(id);
      char line[80];
      if (t && c.available(*t) > 0) snprintf(line, sizeof line, "%s  (%d B)", name, c.available(*t));
      else snprintf(line, sizeof line, "%s", name);
      lv_obj_t* bt = button(b, line, pickContactCb, (void*)(intptr_t)i);
      lv_obj_set_width(bt, LV_PCT(100));
      lv_obj_set_style_text_align(lv_obj_get_child(bt, 0), LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
      lv_obj_align(lv_obj_get_child(bt, 0), LV_ALIGN_LEFT_MID, 0, 0);
      shown++;
    }
    if (!shown) para(b, TR("No chat contacts yet. Add the other person as a contact first."));
  } else {
    for (int i = 0; i < OtpMessenger::channelCount(); i++) {
      char name[33];
      if (!OtpMessenger::channelAt(i, name, sizeof name)) continue;
      const bool pub = otp::Core::isPublicChannel(name);
      char line[80];
      snprintf(line, sizeof line, "#%s%s", name, pub ? TR("  (not available)") : "");
      lv_obj_t* bt = button(b, line, pub ? nullptr : pickChannelCb, (void*)(intptr_t)i);
      lv_obj_set_width(bt, LV_PCT(100));
      lv_obj_align(lv_obj_get_child(bt, 0), LV_ALIGN_LEFT_MID, 0, 0);
      if (pub) lv_obj_add_state(bt, LV_STATE_DISABLED);
      shown++;
    }
    if (!shown) para(b, TR("No channels configured. Add a private channel in Chats first."));
    else para(b, TR("Use a private channel: Public uses a key everyone has, so a pad there protects nothing."));
  }
}

// ---- Chat --------------------------------------------------------------------------------
static int sigGrade(uint32_t* color, const char** word) {
  // Average link margin over the last samples, as the Lua app's signal row.
  static float hist[8];
  static int n = 0, idx = 0;
  static uint32_t next_ms = 0;
  if ((int32_t)(millis() - next_ms) >= 0) {
    next_ms = millis() + 3000;
    hist[idx] = radio_driver.getCurrentRSSI() - radio_driver.getNoiseFloor();
    idx = (idx + 1) % 8;
    if (n < 8) n++;
  }
  float m = 0;
  for (int i = 0; i < n; i++) m += hist[i];
  m = n ? m / n : -99;
  if (m >= 20) { *color = 0x39FF14; *word = "excellent"; return 4; }
  if (m >= 12) { *color = 0x2ECC71; *word = "good"; return 3; }
  if (m >= 6) { *color = 0xF1C40F; *word = "fair"; return 2; }
  if (m >= 0) { *color = 0xE67E22; *word = "poor"; return 1; }
  *color = 0xE74C3C; *word = n ? "very poor" : "no data";
  return 0;
}

static void refreshInfo() {
  otp::Target* t = cur();
  if (!t || !s_info) return;
  otp::Core& c = OtpMessenger::core();
  char txt[200];
  uint32_t col = th().padinfo;
  const int avail = c.available(*t);
  if (t->badge[0]) { snprintf(txt, sizeof txt, "%s", t->badge); col = th().ok; }
  else if (avail <= 0) { snprintf(txt, sizeof txt, "%s", c.hasPad(*t) ? TR("Pad used up") : TR("No pad yet: tap Pad")); col = th().encrypt; }
  else if (avail < otp::kLowPadBytes) { snprintf(txt, sizeof txt, "%d B left: running low", avail); col = th().keyphrase; }
  else snprintf(txt, sizeof txt, "%d B left", avail);
  // sync state + signal on the same line
  const char* sync = t->sync_ok == 1 ? LV_SYMBOL_OK : t->sync_ok == 0 ? LV_SYMBOL_WARNING : "";
  uint32_t scol; const char* sword;
  sigGrade(&scol, &sword);
  char line[260];
  snprintf(line, sizeof line, "#%06lX %s#  #%06lX %s#  #%06lX %s#", (unsigned long)col, txt,
           (unsigned long)(t->sync_ok == 0 ? th().encrypt : th().ok), sync, (unsigned long)scol, sword);
  lv_label_set_text(s_info, line);
  // progress
  if (s_bar) {
    int total = 0;
    int done = c.sendProgress(*t, &total);
    uint32_t bc = th().channel;
    if (done < 0) { done = c.recvProgress(*t, &total); bc = th().ok; }
    if (done >= 0 && total > 1) {
      lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
      lv_bar_set_range(s_bar, 0, total);
      lv_bar_set_value(s_bar, done, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(s_bar, lv_color_hex(bc), LV_PART_INDICATOR);
    } else {
      lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (s_parts) {
    if (t->kind == otp::Kind::Channel && c.show_participants) {
      otp::Participant ps[8];
      const int n = c.activeParticipants(*t, ps, 8);
      char pl[300] = "";
      size_t o = 0;
      for (int i = 0; i < n && o + 48 < sizeof pl; i++)
        o += snprintf(pl + o, sizeof pl - o, "#%06lX %s#  ", (unsigned long)(ps[i].in_sync == 1 ? th().ok : th().ready), ps[i].name);
      if (!n) snprintf(pl, sizeof pl, "#%06lX %s#", (unsigned long)th().ready, TR("no one heard yet"));
      lv_label_set_text(s_parts, pl);
      lv_obj_clear_flag(s_parts, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_parts, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void refreshCounter() {
  otp::Target* t = cur();
  if (!t || !s_ta || !s_counter) return;
  otp::Core& c = OtpMessenger::core();
  const char* text = lv_textarea_get_text(s_ta);
  const int typed = (int)strlen(text ? text : "");
  const int name_len = (int)strlen(the_mesh.getNodePrefs()->node_name);
  const int avail = c.available(*t);
  char buf[96];
  bool bad = false;
  if (t->kind == otp::Kind::Channel) {
    const int cap = otp::maxChannelBytes(name_len);
    snprintf(buf, sizeof buf, "%d / %d  ·  %d B pad", typed, cap, avail);
    bad = typed > cap || typed > avail;
  } else {
    const int ccap = otp::maxChunkBytes(name_len);
    const int parts = typed ? (typed + ccap - 1) / ccap : 0;
    const int need = typed + parts * 12;
    if (parts > 1) snprintf(buf, sizeof buf, "%d typed  ·  %d parts  ·  %d of %d B", typed, parts, need, avail);
    else snprintf(buf, sizeof buf, "%d typed  ·  %d of %d B", typed, need, avail);
    bad = need > avail;
  }
  lv_label_set_text(s_counter, buf);
  lv_obj_set_style_text_color(s_counter, lv_color_hex(bad ? th().encrypt : th().padinfo), LV_PART_MAIN);
}

static void sendNow() {
  otp::Target* t = cur();
  if (!t || !s_ta) return;
  kbMirrorSyncToReal();
  const char* text = lv_textarea_get_text(s_ta);
  if (!text || !*text) return;
  char err[160];
  const otp::SendResult r = OtpMessenger::core().send(*t, text, strlen(text), err, sizeof err);
  if (r != otp::SendResult::Ok) { toast(err, 2600); return; }
  setTextareaSynced(s_ta, "");
  refreshCounter();
}

static void buildBubbles(bool keep_scroll) {
  otp::Target* t = cur();
  if (!t || !s_list) return;
  otp::Core& c = OtpMessenger::core();
  c.ensureLog(*t);
  const lv_coord_t old_y = lv_obj_get_scroll_y(s_list);
  const lv_coord_t old_bottom = lv_obj_get_scroll_bottom(s_list);
  lv_obj_clean(s_list);
  const lv_coord_t w = lv_obj_get_content_width(s_list);
  const lv_coord_t max_w = w * 4 / 5;
  const int first = t->log_n > s_shown_n ? t->log_n - s_shown_n : 0;
  if (first > 0) {
    lv_obj_t* more = button(s_list, TR("Earlier messages"), [](lv_event_t*) {
      s_shown_n += 60;
      lv_async_call([](void*) { buildBubbles(false); }, nullptr);   // not from inside the list being rebuilt
    }, nullptr, 0, 24);
    lv_obj_set_style_align(more, LV_ALIGN_CENTER, LV_PART_MAIN);
  }
  if (!t->log_n) {
    para(s_list, c.hasPad(*t) ? TR("No messages yet. Everything you send here is one-time-pad encrypted.")
                              : TR("There is no pad for this conversation yet. Open Pad to generate or import one."));
  }
  // OTP_3_RC1: own messages on the LEFT, theirs on the right.
  for (int i = first; i < t->log_n; i++) {
    const otp::Entry& e = t->log[i];
    const bool own = e.mine;
    uint32_t outline = th().plaintext;
    switch (e.state) {
      case otp::State::Sent: case otp::State::Pending: outline = th().channel; break;
      case otp::State::Delivered: outline = th().ok; break;
      case otp::State::Retrying: outline = th().keyphrase; break;
      case otp::State::Waiting: outline = th().ready; break;
      case otp::State::Received: outline = senderColor(e.who[0] ? e.who : "Them"); break;
      case otp::State::Failed: case otp::State::Lost: outline = th().encrypt; break;
    }
    const uint32_t fill = (e.state == otp::State::Pending || e.state == otp::State::Retrying) ? darken(outline, 0.6f) : outline;
    lv_obj_t* wrap = lv_obj_create(s_list);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrap, own ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END,
                          own ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END, own ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(wrap, 1, LV_PART_MAIN);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* bub = lv_obj_create(wrap);
    lv_obj_remove_style_all(bub);
    lv_obj_set_size(bub, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bub, max_w, LV_PART_MAIN);
    styleSurface(bub, fill, 8);
    lv_obj_set_style_border_width(bub, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(bub, lv_color_hex(lighten(outline, 0.35f)), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bub, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bub, 4, LV_PART_MAIN);
    lv_obj_clear_flag(bub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* tl = lv_label_create(bub);
    lv_label_set_text(tl, e.text ? e.text : "");
    lv_obj_set_style_text_font(tl, &g_font_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(tl, lv_color_hex(contrastOf(fill)), LV_PART_MAIN);
    useChainedFont(tl);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(tl, max_w - 14, LV_PART_MAIN);
    // caption: "Name · 12:04" or "Delivered · 12:04"
    char when[24], cap[96], sw[48];
    fmtAgo(e.at_epoch, e.at_ms, when, sizeof when);
    if (!own) {
      if (t->kind == otp::Kind::Channel || e.state != otp::State::Received) snprintf(cap, sizeof cap, "%s · %s", e.who[0] ? e.who : "Them", when);
      else snprintf(cap, sizeof cap, "%s", when);
    } else {
      const char* st = c.statusWord(*t, e, sw, sizeof sw);
      if (st && *st) snprintf(cap, sizeof cap, "%s · %s", st, when);
      else snprintf(cap, sizeof cap, "%s", when);
    }
    lv_obj_t* cl = label(wrap, cap, &g_font_12, th().padinfo);
    useChainedFont(cl);
  }
  lv_obj_update_layout(s_list);
  if (keep_scroll && old_bottom > 4) lv_obj_scroll_to_y(s_list, old_y, LV_ANIM_OFF);
  else lv_obj_scroll_to_y(s_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void buildChat() {
  otp::Target* t = cur();
  if (!t) { show(Screen::List); return; }
  otp::Core& c = OtpMessenger::core();
  c.setOpenTarget(t);
  s_shown_n = 60;
  lv_obj_t* b = newBody(false);
  const lv_coord_t W = lv_obj_get_width(s_root), H = lv_obj_get_height(s_root);
  // header
  lv_obj_t* hdr = lv_obj_create(b);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_size(hdr, W, 26);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  char title[48];
  snprintf(title, sizeof title, "%s%s", t->kind == otp::Kind::Channel ? "#" : "", t->name);
  lv_obj_t* tl = label(hdr, title, &g_font_16, t->kind == otp::Kind::Channel ? th().channel : senderColor(t->name));
  useChainedFont(tl);
  lv_obj_set_width(tl, W - 150);
  lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
  lv_obj_align(tl, LV_ALIGN_LEFT_MID, 6, 0);
  lv_obj_t* pad = button(hdr, TR("Pad"), [](lv_event_t*) { show(Screen::Pad); }, nullptr, 0, 24);
  lv_obj_align(pad, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_t* sync = button(hdr, LV_SYMBOL_REFRESH, [](lv_event_t*) {
    if (otp::Target* x = cur()) { OtpMessenger::core().requestSyncCheck(*x); toast(TR("Sync check sent"), 1200); }
  }, nullptr, 0, 24);
  lv_obj_align_to(sync, pad, LV_ALIGN_OUT_LEFT_MID, -4, 0);
  if (t->kind == otp::Kind::Channel) {
    lv_obj_t* pp = button(hdr, LV_SYMBOL_EYE_OPEN, [](lv_event_t*) {
      otp::Core& cc = OtpMessenger::core();
      cc.show_participants = !cc.show_participants;
      cc.saveSettings();
      refreshInfo();
    }, nullptr, 0, 24);
    lv_obj_align_to(pp, sync, LV_ALIGN_OUT_LEFT_MID, -4, 0);
  }
  // info line
  s_info = lv_label_create(b);
  lv_label_set_recolor(s_info, true);
  lv_obj_set_style_text_font(s_info, &g_font_12, LV_PART_MAIN);
  lv_obj_set_width(s_info, W - 12);
  lv_label_set_long_mode(s_info, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(s_info, 6, 27);
  s_parts = lv_label_create(b);
  lv_label_set_recolor(s_parts, true);
  lv_obj_set_style_text_font(s_parts, &g_font_12, LV_PART_MAIN);
  lv_obj_set_width(s_parts, W - 12);
  lv_label_set_long_mode(s_parts, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(s_parts, 6, 42);
  s_bar = lv_bar_create(b);
  lv_obj_set_size(s_bar, W - 12, 4);
  lv_obj_set_pos(s_bar, 6, 42);
  lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
  lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
  // composer
  const lv_coord_t comp_h = 50;
  s_ta = lv_textarea_create(b);
  lv_obj_set_size(s_ta, W - 70, 32);
  lv_obj_set_pos(s_ta, 4, H - comp_h);
  styleCard(s_ta);
  lv_textarea_set_one_line(s_ta, true);
  lv_textarea_set_max_length(s_ta, t->kind == otp::Kind::Channel ? 65 : 2000);
  lv_obj_set_style_text_font(s_ta, &g_font_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_ta, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  taSetPlaceholder(s_ta, c.hasPad(*t) ? TR("Encrypted message") : TR("No pad yet"));
  attachSettingsTaEvents(s_ta);
  lv_obj_add_event_cb(s_ta, [](lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) sendNow();
    else if (code == LV_EVENT_VALUE_CHANGED) refreshCounter();
    else if (code == LV_EVENT_FOCUSED) { if (otp::Target* x = cur()) x->next_sync_check = 0; }
  }, LV_EVENT_ALL, nullptr);
  s_send_btn = button(b, LV_SYMBOL_RIGHT, [](lv_event_t*) { sendNow(); }, nullptr, th().channel, 32);
  lv_obj_set_width(s_send_btn, 56);
  lv_obj_set_pos(s_send_btn, W - 62, H - comp_h);
  s_counter = label(b, "", &g_font_12, th().padinfo);
  lv_obj_set_pos(s_counter, 8, H - 16);
  // bubbles fill the middle
  s_list = lv_obj_create(b);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_pos(s_list, 0, 48);
  lv_obj_set_size(s_list, W, H - 48 - comp_h - 4);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(s_list, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_list, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  refreshInfo();
  refreshCounter();
  buildBubbles(false);
  if (g_lv.keyboard) kbMirrorBind(s_ta);
}

// ---- Pad --------------------------------------------------------------------------------
static void importHexText(const char* text) {
  otp::Target* t = cur();
  if (!t || !text) return;
  char err[160];
  if (!OtpMessenger::core().importHex(*t, text, strlen(text), err, sizeof err)) { toast(err, 2600); return; }
  char msg[64]; snprintf(msg, sizeof msg, "Imported %d bytes", OtpMessenger::core().available(*t));
  toast(msg);
  show(Screen::Pad);
}
static void addTextBytes(const char* text) {
  otp::Target* t = cur();
  if (!t || !text) return;
  char err[160];
  if (!OtpMessenger::core().addBytes(*t, (const uint8_t*)text, strlen(text), err, sizeof err)) { toast(err, 2600); return; }
  show(Screen::Pad);
}
static void wipeConfirmed() {
  if (otp::Target* t = cur()) { OtpMessenger::core().wipe(*t); toast(TR("Pad wiped")); show(Screen::Pad); }
}
static void forgetConfirmed() {
  if (otp::Target* t = cur()) OtpMessenger::core().forget(*t);
  s_tid[0] = 0;
  show(Screen::List);
}
static void exportPadNow() {
  otp::Target* t = cur();
  if (!t) return;
  fs::FS* sd = backupSdFs();
  if (!sd) { toast(TR("No SD card"), 1800); return; }
  char path[64];
  char safe[33];
  size_t j = 0;
  for (const char* p = t->name; *p && j < 24; ++p) {
    const char ch = *p;
    safe[j++] = ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) ? ch : '_';
  }
  safe[j] = 0;
  snprintf(path, sizeof path, "/otp_%s.hex", safe);
  if (!OtpMessenger::exportPad(*t, sd, path)) {
    toast(TR("Only an unused pad can be exported"), 2400);
    return;
  }
  char msg[96];
  snprintf(msg, sizeof msg, "Saved %s to the SD card. Import it on the other device, then delete it.", path);
  toast(msg, 3500);
}

static void buildPad() {
  otp::Target* t = cur();
  if (!t) { show(Screen::List); return; }
  otp::Core& c = OtpMessenger::core();
  c.setOpenTarget(t);
  lv_obj_t* b = newBody();
  char title[64];
  snprintf(title, sizeof title, "%s %s%s", TR("Pad:"), t->kind == otp::Kind::Channel ? "#" : "", t->name);
  heading(b, title);
  const int avail = c.available(*t);
  char line[200];
  if (!c.hasPad(*t)) snprintf(line, sizeof line, "%s", TR("No pad yet."));
  else if (avail <= 0) snprintf(line, sizeof line, "%s", TR("This pad is used up. Wipe it and add a new one."));
  else snprintf(line, sizeof line, "%d bytes left", avail);
  lv_obj_t* st = para(b, line, avail <= 0 ? th().encrypt : th().padinfo);
  lv_obj_set_style_text_font(st, &g_font_14, LV_PART_MAIN);
  if (t->kind == otp::Kind::Dm) {
    const char ar = c.autoRole(*t);
    snprintf(line, sizeof line, "Role %c%s  ·  sent %u B  ·  received %u B", t->role,
             ar == t->role ? " (auto)" : " (manual)", (unsigned)t->ctr_mine, (unsigned)t->ctr_theirs);
  } else {
    snprintf(line, sizeof line, "Channel offset %u  ·  shared by everyone on #%s", (unsigned)t->ctr, t->name);
  }
  para(b, line);
  snprintf(line, sizeof line, "Sync: %s", t->sync_text[0] ? t->sync_text : TR("not checked yet"));
  para(b, line, t->sync_ok == 1 ? th().ok : t->sync_ok == 0 ? th().encrypt : COLOR_SUB);
  if (t->wq_n) {
    snprintf(line, sizeof line, "%d message(s) waiting for a path", t->wq_n);
    para(b, line, th().keyphrase);
  }

  lv_obj_t* r1 = row(b);
  button(r1, TR("Open chat"), [](lv_event_t*) { show(Screen::Chat); }, nullptr, th().channel);
  button(r1, TR("Check sync"), [](lv_event_t*) {
    if (otp::Target* x = cur()) { OtpMessenger::core().requestSyncCheck(*x); show(Screen::Pad); }
  });

  para(b, TR("Add pad bytes"), COLOR_TEXT);
  lv_obj_t* r2 = row(b);
  button(r2, TR("Generate 1000 B"), [](lv_event_t*) {
    otp::Target* x = cur();
    if (!x) return;
    const int n = OtpMessenger::core().generate(*x, 1000);
    char msg[80];
    if (n > 0) snprintf(msg, sizeof msg, "Added %d random bytes (hardware RNG)", n);
    else snprintf(msg, sizeof msg, "The pad is at the %d byte cap", otp::kMaxTotalPadBytes);
    toast(msg);
    show(Screen::Pad);
  });
  button(r2, TR("Import file"), [](lv_event_t*) { show(Screen::Import); });
  button(r2, TR("Paste hex"), [](lv_event_t*) {
    prompt(TR("Pad as hex"), TR("Spaces and line breaks are ignored."), false, 16400, importHexText);
  });
  button(r2, TR("Add text"), [](lv_event_t*) {
    prompt(TR("Pad text"), TR("Typed characters become pad bytes as-is. Both sides must add exactly the same text."),
           false, 2000, addTextBytes);
  });

  para(b, TR("Share"), COLOR_TEXT);
  lv_obj_t* r3 = row(b);
  button(r3, TR("Export to SD"), [](lv_event_t*) { exportPadNow(); });
  if (t->kind == otp::Kind::Dm) {
    button(r3, TR("Flip role"), [](lv_event_t*) {
      if (otp::Target* x = cur()) { OtpMessenger::core().flipRole(*x); show(Screen::Pad); }
    });
  }
  para(b, TR("Export writes this pad, unused, to the SD card as hex. The other device imports the same file; "
             "roles A/B are worked out from your keys, so both sides import it unchanged."));

  para(b, TR("Danger zone"), th().encrypt);
  lv_obj_t* r4 = row(b);
  if (t->wq_n) {
    button(r4, TR("Cancel waiting"), [](lv_event_t*) {
      if (otp::Target* x = cur()) { OtpMessenger::core().cancelWaiting(*x); show(Screen::Pad); }
    });
  }
  button(r4, TR("Wipe pad"), [](lv_event_t*) {
    showConfirm(TR("Wipe this pad? Messages already received stay readable; nothing new can be sent or read until a new pad is added."),
                TR("Wipe"), wipeConfirmed);
  }, nullptr, COLOR_STATUS_DANGER);
  button(r4, TR("Delete conversation"), [](lv_event_t*) {
    showConfirm(TR("Delete the pad and every message of this conversation?"), TR("Delete"), forgetConfirmed);
  }, nullptr, COLOR_STATUS_DANGER);
}

// ---- Import from SD -------------------------------------------------------------------------
static char s_import_names[24][40];
static int s_import_n = 0;
static void importFileCb(lv_event_t* e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  otp::Target* t = cur();
  fs::FS* sd = backupSdFs();
  if (!t || !sd || i < 0 || i >= s_import_n) return;
  File f = sd->open(s_import_names[i], "r");
  if (!f) { toast(TR("Could not open the file")); return; }
  const size_t n = f.size();
  if (n > 40000) { f.close(); toast(TR("File too large for a pad")); return; }
  char* buf = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { f.close(); toast(TR("Out of memory")); return; }
  const size_t rd = f.read((uint8_t*)buf, n);
  f.close();
  buf[rd] = 0;
  char err[160];
  bool ok;
  const char* nm = s_import_names[i];
  const size_t nl = strlen(nm);
  const bool is_txt = nl > 4 && !strcasecmp(nm + nl - 4, ".txt");
  if (is_txt) ok = OtpMessenger::core().addBytes(*t, (const uint8_t*)buf, rd, err, sizeof err) ||
                   false;
  else ok = OtpMessenger::core().importHex(*t, buf, rd, err, sizeof err);
  memset(buf, 0, n + 1);
  heap_caps_free(buf);
  if (!ok) { toast(err, 2600); return; }
  if (!OtpMessenger::core().keep_files) sd->remove(nm);
  char msg[96];
  snprintf(msg, sizeof msg, "Imported %s. %s", nm, OtpMessenger::core().keep_files ? "Delete the file when both sides have it." : "File deleted.");
  toast(msg, 2600);
  show(Screen::Pad);
}
static void scanDir(fs::FS* sd, const char* dir) {
  File root = sd->open(dir);
  if (!root || !root.isDirectory()) return;
  for (File f = root.openNextFile(); f && s_import_n < 24; f = root.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }
    const char* nm = f.name();
    const char* base = strrchr(nm, '/');
    base = base ? base + 1 : nm;
    const size_t n = strlen(base);
    if (n > 4 && (!strcasecmp(base + n - 4, ".hex") || !strcasecmp(base + n - 4, ".txt")) && f.size() > 0) {
      snprintf(s_import_names[s_import_n++], sizeof s_import_names[0], "%s%s%s", dir, dir[1] ? "/" : "", base);
    }
    f.close();
  }
  root.close();
}
static void buildImport() {
  otp::Target* t = cur();
  if (!t) { show(Screen::List); return; }
  lv_obj_t* b = newBody();
  heading(b, TR("Import pad file"));
  para(b, TR(".hex files are imported as a new pad (replacing this one). .txt files are added as raw bytes."));
  fs::FS* sd = backupSdFs();
  s_import_n = 0;
  if (!sd) { para(b, TR("No SD card found."), th().encrypt); return; }
  scanDir(sd, "/");
  scanDir(sd, "/otp");
  if (!s_import_n) { para(b, TR("No .hex or .txt files in the SD card's top folder or /otp."), th().keyphrase); return; }
  for (int i = 0; i < s_import_n; i++) {
    lv_obj_t* bt = button(b, s_import_names[i], importFileCb, (void*)(intptr_t)i);
    lv_obj_set_width(bt, LV_PCT(100));
    lv_obj_align(lv_obj_get_child(bt, 0), LV_ALIGN_LEFT_MID, 0, 0);
  }
}

// ---- moving over from the Lua app -----------------------------------------------------------------
#if CAP_LUA_APPS
void luaHostAppPath(char* out, size_t cap, const char* rel);
static char s_lua_dir[96];     // <data>/apps/<id>.d
static char s_lua_sav[96];     // <data>/apps/<id>.sav

static bool luaReadCb(void*, const char* name, otp::Bytes& out) {
  fs::FS* fs = luaHostAppFs();
  if (!fs) return false;
  char path[160];
  snprintf(path, sizeof path, "%s/%s", s_lua_dir, name);
  File f = fs->open(path, "r");
  if (!f) return false;
  const size_t n = f.size();
  if (!out.reserve((uint32_t)n + 1)) { f.close(); return false; }
  const size_t rd = f.read(out.p, n);
  f.close();
  out.n = (uint32_t)rd;
  out.p[rd] = 0;
  return rd == n;
}
static bool luaContactCb(void*, const char* hex8, uint8_t pub[32], char* name, size_t cap) {
  const int n = OtpMessenger::contactCount();
  for (int i = 0; i < n; i++) {
    uint8_t p[32];
    char nm[33];
    if (!OtpMessenger::contactAt(i, p, nm, sizeof nm)) continue;
    char h[9];
    otp::toHex(p, 4, h);
    if (strcasecmp(h, hex8) != 0) continue;
    memcpy(pub, p, 32);
    snprintf(name, cap, "%s", nm);
    return true;
  }
  return false;
}
static bool luaChannelCb(void*, const char* san, char* name, size_t cap) {
  for (int i = 0; i < OtpMessenger::channelCount(); i++) {
    char nm[33], s2[40];
    if (!OtpMessenger::channelAt(i, nm, sizeof nm)) continue;
    size_t j = 0;
    for (const char* c = nm; *c && j + 1 < sizeof s2; ++c)
      s2[j++] = ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')) ? *c : '_';
    s2[j] = 0;
    if (strcmp(s2, san) == 0) { snprintf(name, cap, "%s", nm); return true; }
  }
  return false;
}
// The installed Lua app that holds OTP pads (any id: it was sideloaded).
static bool findLuaApp(otp::Bytes& sav) {
  fs::FS* fs = luaHostAppFs();
  if (!fs) return false;
  char apps[80];
  luaHostAppPath(apps, sizeof apps, "/apps");
  File root = fs->open(apps);
  if (!root || !root.isDirectory()) return false;
  bool found = false;
  for (File f = root.openNextFile(); f && !found; f = root.openNextFile()) {
    const char* nm = f.name();
    const char* base = strrchr(nm, '/');
    base = base ? base + 1 : nm;
    const size_t n = strlen(base);
    if (!f.isDirectory() && n > 4 && !strcmp(base + n - 4, ".sav") && f.size() < 64000) {
      sav.clear();
      if (sav.reserve((uint32_t)f.size() + 1)) {
        sav.n = (uint32_t)f.read(sav.p, f.size());
        sav.p[sav.n] = 0;
        if (strstr((const char*)sav.p, "padmeta_")) {
          snprintf(s_lua_sav, sizeof s_lua_sav, "%s/%s", apps, base);
          snprintf(s_lua_dir, sizeof s_lua_dir, "%s/%.*s.d", apps, (int)(n - 4), base);
          found = true;
        }
      }
    }
    f.close();
  }
  root.close();
  return found;
}
static void luaImportWith(const char* pass) {
  if (!pass) return;
  otp::Bytes sav;
  if (!findLuaApp(sav)) { toast(TR("No OTP pads from the Lua app found"), 2200); return; }
  otp::Core::LuaSource src{luaReadCb, luaContactCb, luaChannelCb, nullptr};
  char err[120];
  const int n = OtpMessenger::core().importLua((const char*)sav.p, sav.n, src, pass, err, sizeof err);
  if (n < 0) { toast(err, 2400); return; }
  char msg[120];
  if (n) snprintf(msg, sizeof msg, "Moved %d pad(s), counters included. Remove the Lua app so only one copy is used.", n);
  else snprintf(msg, sizeof msg, "%s", TR("Nothing to move: those pads are already here, or their contacts are unknown"));
  toast(msg, 3500);
  show(Screen::List);
}
static void luaImport() {
  otp::Bytes sav;
  if (!findLuaApp(sav)) { toast(TR("No OTP pads from the Lua app found"), 2200); return; }
  if (otp::Core::luaNeedsPassphrase((const char*)sav.p, sav.n))
    prompt(TR("Lua app passphrase"), TR("The Lua app's pads are protected. Its passphrase unlocks them once, to move them."),
           true, 64, luaImportWith);
  else luaImportWith("");
}
#endif

// ---- Settings -----------------------------------------------------------------------------------
static char s_pass1[65];
static void passSecond(const char* p) {
  if (!p) { memset(s_pass1, 0, sizeof s_pass1); return; }
  if (strcmp(p, s_pass1) != 0) { memset(s_pass1, 0, sizeof s_pass1); toast(TR("Passphrases did not match"), 1800); return; }
  const bool ok = OtpMessenger::core().enableProtection(p);
  memset(s_pass1, 0, sizeof s_pass1);
  toast(ok ? TR("Protection on. Pads and messages are now encrypted.") : TR("Could not enable protection"), 2400);
  show(Screen::Settings);
}
static void passFirst(const char* p) {
  if (!p) return;
  if (!*p) { toast(TR("The passphrase cannot be empty")); return; }
  snprintf(s_pass1, sizeof s_pass1, "%s", p);
  prompt(TR("Confirm passphrase"), nullptr, true, 64, passSecond);
}
static void buildSettings() {
  otp::Core& c = OtpMessenger::core();
  lv_obj_t* b = newBody();
  heading(b, TR("Settings"));
  char line[96];
  lv_obj_t* r = row(b);
  snprintf(line, sizeof line, "%s: %s", TR("Theme"), th().name);
  button(r, line, [](lv_event_t*) {
    otp::Core& cc = OtpMessenger::core();
    cc.theme = (uint8_t)((cc.theme + 1) % 6);
    cc.saveSettings();
    show(Screen::Settings);
  }, nullptr, th().channel);
  snprintf(line, sizeof line, "%s: %s", TR("Sound"), c.sound ? TR("on") : TR("off"));
  button(r, line, [](lv_event_t*) {
    otp::Core& cc = OtpMessenger::core();
    cc.sound = !cc.sound;
    cc.saveSettings();
    show(Screen::Settings);
  });
  snprintf(line, sizeof line, "%s: %s", TR("Keep imported files"), c.keep_files ? TR("yes") : TR("no"));
  button(r, line, [](lv_event_t*) {
    otp::Core& cc = OtpMessenger::core();
    cc.keep_files = !cc.keep_files;
    cc.saveSettings();
    show(Screen::Settings);
  });
  // theme swatch
  lv_obj_t* sw = row(b, 3);
  const Theme& t = th();
  const uint32_t cols[] = {t.encrypt, t.decrypt, t.ciphertext, t.plaintext, t.newpad, t.channel, t.ok, t.keyphrase, t.padinfo, t.ready};
  for (uint32_t col : cols) {
    lv_obj_t* d = lv_obj_create(sw);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 18, 12);
    styleSurface(d, col, 3);
  }

  para(b, TR("Passphrase lock"), COLOR_TEXT);
  if (c.protectionEnabled()) {
    para(b, TR("On: pads, messages and the conversation list are encrypted on this device."), th().ok);
    lv_obj_t* r2 = row(b);
    button(r2, TR("Lock now"), [](lv_event_t*) {
      OtpMessenger::core().lockNow();
      s_tid[0] = 0;
      show(Screen::Lock);
    }, nullptr, th().channel);
  } else {
    para(b, TR("Off. With a passphrase, everything is encrypted on this device and the app asks for it "
               "after every restart. Incoming messages wait until you unlock."));
    button(b, TR("Set passphrase"), [](lv_event_t*) { prompt(TR("New passphrase"), nullptr, true, 64, passFirst); }, nullptr, th().channel);
  }
  button(b, TR("Panic wipe"), [](lv_event_t*) {
    showConfirm(TR("Erase every pad and message on this device, and turn protection off?\n\nThis cannot be undone."),
                TR("Wipe"), panicConfirmed);
  }, nullptr, COLOR_STATUS_DANGER);
#if CAP_LUA_APPS
  para(b, TR("Coming from the Lua app"), COLOR_TEXT);
  para(b, TR("Moves its pads here with their counters, so you carry on in step with the other side. "
             "Afterwards remove the Lua app: two apps using one pad would burn the same bytes twice."));
  button(b, TR("Move pads from the Lua app"), [](lv_event_t*) { luaImport(); });
#endif
  snprintf(line, sizeof line, "OTP Messenger %s · wire-compatible with OTP_3_RC1 and MeshCore Open TAK", otp::Core::kBuildId);
  para(b, line);
}

// ---- navigation -----------------------------------------------------------------------------------
static void show(Screen s) {
  if (!s_root) return;
  promptClose();
  otp::Core& c = OtpMessenger::core();
  if (c.locked()) s = Screen::Lock;
  if (s != Screen::Chat && s != Screen::Pad) c.setOpenTarget(nullptr);
  if (s != Screen::Chat) hideKb();
  s_screen = s;
  switch (s) {
    case Screen::Lock: buildLock(); break;
    case Screen::List: buildList(); break;
    case Screen::Pick: buildPick(); break;
    case Screen::Chat: buildChat(); break;
    case Screen::Pad: buildPad(); break;
    case Screen::Import: buildImport(); break;
    case Screen::Settings: buildSettings(); break;
  }
  s_seen = OtpMessenger::version();
}

static void back() {
  if (s_prompt) { promptDone(false); return; }
  switch (s_screen) {
    case Screen::Chat: show(Screen::List); break;
    case Screen::Pad: show(cur() && OtpMessenger::core().hasPad(*cur()) ? Screen::Chat : Screen::List); break;
    case Screen::Import: show(Screen::Pad); break;
    case Screen::Pick: case Screen::Settings: show(Screen::List); break;
    default: close(); break;
  }
}

static void timerCb(lv_timer_t*) {
  if (!s_root) return;
  const uint32_t v = OtpMessenger::version();
  if (v != s_seen) {
    s_seen = v;
    if (s_screen == Screen::Chat) { refreshInfo(); refreshCounter(); buildBubbles(true); }
    else if (s_screen == Screen::List) buildList();
  } else if (s_screen == Screen::Chat) {
    refreshInfo();   // signal grade, badge expiry
  }
}

static void close() {
  promptClose();
  if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
  if (OtpMessenger::ready()) OtpMessenger::core().setOpenTarget(nullptr);
  hideKb();
  s_body = s_list = s_ta = s_counter = s_info = s_parts = s_bar = s_send_btn = nullptr;
  if (s_root) { lv_obj_t* r = s_root; s_root = nullptr; appPageDeleteRootAsync(r); }
  appPageEnd(back);
}

static void open() {
  if (!OtpMessenger::ready()) { toast(TR("Storage not ready yet")); return; }
  if (s_root) return;
  s_root = appPageCreateRoot(COLOR_BG);
  appPageBeginSlim("OTP Messenger", back);
  s_timer = lv_timer_create(timerCb, 500, nullptr);
  otp::Target* t = cur();
  show(t ? Screen::Chat : Screen::List);
  if (g_statusbar.root) lv_obj_move_foreground(g_statusbar.root);
}

static bool isOpen() { return s_root != nullptr; }

// ---- background callbacks -------------------------------------------------------------------------
static void toastCb(const char* m) { toast(m, 2200); }
static void incomingCb(otp::Target& t, const otp::Entry& e) {
  const bool showing = s_root && s_screen == Screen::Chat && !strcmp(s_tid, t.id);
  if (showing) return;
  // Name only: the text stays inside the app.
  char msg[64];
  snprintf(msg, sizeof msg, "OTP: %s%s", t.kind == otp::Kind::Channel ? "#" : "", e.who[0] ? e.who : t.name);
  if (t.kind == otp::Kind::Channel) snprintf(msg, sizeof msg, "OTP #%s: %s", t.name, e.who);
  toast(msg, 2500);
  if (OtpMessenger::core().sound && g_lv.task && !g_lv.task->isBuzzerQuiet()) uiPlaySlot(TOUCH_SND_MSG);
}

}  // namespace otpui
