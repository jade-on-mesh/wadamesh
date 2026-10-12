// SPDX-License-Identifier: GPL-3.0-or-later
// Interop test: the native OTP core (src/ui-touch/OtpCore.cpp) against Jade's
// OTP_3_RC1.lua, both running here and talking over a simulated mesh. The Lua
// app runs on the firmware's own vendored Lua (LUA_32BITS) with a stub wada.*.
//
//   test/build_otp_test.sh
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "ui-touch/OtpCore.h"

using otp::Bytes;

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } \
  } while (0)

// ---- shared world -----------------------------------------------------------
static uint32_t g_now = 100000;
static uint32_t g_rng = 0x12345678;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static uint8_t kLuaPub[32], kNativePub[32];
static const char* kLuaName = "LuaNode";
static const char* kNativeName = "Native";

struct Air { bool to_native; bool dm; std::string chan, text; };
static std::deque<Air> g_air;
static int g_drop_lua_channel = 0;   // drop the next N Lua->native channel posts
static bool g_verbose = getenv("VERBOSE") != nullptr;

static std::string hex4(const uint8_t* p) { char h[9]; otp::toHex(p, 4, h); return h; }

// ---- native side ------------------------------------------------------------
struct TestIo : otp::Io {
  std::map<std::string, std::string> files;
  std::vector<std::string> toasts, incoming_texts;
  int sends = 0;
  uint32_t nowMs() override { return g_now; }
  uint32_t epoch() override { return 1760000000u + g_now / 1000; }
  uint8_t randomByte() override { return (uint8_t)rnd(); }
  bool airtimeClear() override { return true; }
  const char* selfName() override { return kNativeName; }
  bool selfPub(uint8_t out[32]) override { memcpy(out, kNativePub, 32); return true; }
  bool sendDm(const uint8_t pub[32], const char* text) override {
    assert(memcmp(pub, kLuaPub, 32) == 0);
    sends++;
    g_air.push_back({false, true, "", text});
    if (g_verbose) printf("  native -> lua DM  %s\n", text);
    return true;
  }
  bool sendChannel(const char* name, const char* text, uint32_t* fp) override {
    sends++;
    g_air.push_back({false, false, name, text});
    if (fp) *fp = 77;
    if (g_verbose) printf("  native -> lua #%s %s\n", name, text);
    return true;
  }
  uint8_t repeatsFor(uint32_t) override { return 2; }
  bool readFile(const char* n, Bytes& out) override {
    auto it = files.find(n);
    if (it == files.end()) return false;
    out.assign(it->second.data(), (uint32_t)it->second.size());
    return true;
  }
  bool writeFile(const char* n, const uint8_t* d, uint32_t len) override {
    files[n] = std::string((const char*)d, len);
    return true;
  }
  bool removeFile(const char* n) override { files.erase(n); return true; }
  void removeAll() override { files.clear(); }
  void toast(const char* m) override { toasts.push_back(m); if (g_verbose) printf("  [native toast] %s\n", m); }
  void changed(otp::Target&) override {}
  void incoming(otp::Target&, const otp::Entry& e) override { incoming_texts.push_back(std::string(e.text, e.len)); }
};

// ---- Lua side -----------------------------------------------------------------
static lua_State* L = nullptr;

static int hMillis(lua_State* l) { lua_pushinteger(l, (lua_Integer)g_now); return 1; }
static int hRandom(lua_State* l) { lua_pushinteger(l, (lua_Integer)(rnd() & 0x7FFFFFFF)); return 1; }
static int hHex(lua_State* l) {
  size_t n; const char* s = luaL_checklstring(l, 1, &n);
  std::string out(n * 2 + 1, 0);
  otp::toHex((const uint8_t*)s, n, &out[0]);
  lua_pushlstring(l, out.data(), n * 2);
  return 1;
}
static int hHmac(lua_State* l) {
  size_t kn, mn;
  const char* k = luaL_checklstring(l, 1, &kn);
  const char* m = luaL_checklstring(l, 2, &mn);
  uint8_t out[32];
  otp::hmacSha256((const uint8_t*)k, kn, (const uint8_t*)m, mn, out);
  lua_pushlstring(l, (const char*)out, 32);
  return 1;
}
static int hSendDm(lua_State* l) {
  const char* to = luaL_checkstring(l, 1);
  const char* text = luaL_checkstring(l, 2);
  if (strcmp(to, kNativeName) != 0) { lua_pushboolean(l, 0); return 1; }
  g_air.push_back({true, true, "", text});
  if (g_verbose) printf("  lua -> native DM  %s\n", text);
  lua_pushboolean(l, 1);
  return 1;
}
static int hSend(lua_State* l) {
  const char* ch = luaL_checkstring(l, 1);
  const char* text = luaL_checkstring(l, 2);
  if (g_drop_lua_channel > 0) {
    g_drop_lua_channel--;
    if (g_verbose) printf("  lua -> native #%s %s   (DROPPED)\n", ch, text);
  } else {
    g_air.push_back({true, false, ch, text});
    if (g_verbose) printf("  lua -> native #%s %s\n", ch, text);
  }
  lua_pushboolean(l, 1);
  return 1;
}

static void lrun(const char* code) {
  if (luaL_dostring(L, code) != LUA_OK) {
    fprintf(stderr, "lua error: %s\n  in: %s\n", lua_tostring(L, -1), code);
    lua_pop(L, 1);
    g_fail++;
  }
}
static long long lnum(const char* expr) {
  std::string code = std::string("return ") + expr;
  if (luaL_dostring(L, code.c_str()) != LUA_OK) {
    fprintf(stderr, "lua error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    g_fail++;
    return -1;
  }
  long long v = lua_isnumber(L, -1) ? (long long)lua_tointeger(L, -1) : -1;
  lua_pop(L, 1);
  return v;
}
static std::string lstr(const char* expr) {
  std::string code = std::string("return tostring(") + expr + ")";
  if (luaL_dostring(L, code.c_str()) != LUA_OK) { lua_pop(L, 1); return "<err>"; }
  std::string v = lua_tostring(L, -1);
  lua_pop(L, 1);
  return v;
}

static void luaStart(const char* app_path, const char* mock_path) {
  L = luaL_newstate();
  luaL_openlibs(L);
  lua_pushcfunction(L, hMillis); lua_setglobal(L, "host_millis");
  lua_pushcfunction(L, hRandom); lua_setglobal(L, "host_random");
  lua_pushcfunction(L, hHex); lua_setglobal(L, "host_hex");
  lua_pushcfunction(L, hHmac); lua_setglobal(L, "host_hmac");
  lua_pushcfunction(L, hSendDm); lua_setglobal(L, "host_send_dm");
  lua_pushcfunction(L, hSend); lua_setglobal(L, "host_send");
  lua_pushstring(L, kLuaName); lua_setglobal(L, "SELF_NAME");
  lua_pushstring(L, hex4(kLuaPub).c_str()); lua_setglobal(L, "SELF_PK");
  lua_pushstring(L, kNativeName); lua_setglobal(L, "PEER_NAME");
  lua_pushstring(L, hex4(kNativePub).c_str()); lua_setglobal(L, "PEER_PK");
  lua_pushstring(L, "ops"); lua_setglobal(L, "CHANNEL");
  if (g_verbose) { lua_pushboolean(L, 1); lua_setglobal(L, "VERBOSE"); }
  if (luaL_dofile(L, mock_path) != LUA_OK) { fprintf(stderr, "mock: %s\n", lua_tostring(L, -1)); exit(1); }
  lrun("STORE.otp_dk = 'dm'; STORE.otp_dm = PEER_NAME");
  std::string code = std::string("APP = dofile('") + app_path + "')";
  lrun(code.c_str());
  lrun("APP.on_open(320, 240)");
  // Record what the Lua app shows in its chat, for checks.
  lrun("SHOWN = {}; local orig = a_chat_for; a_chat_for = function(id, who, text, kind) "
       "SHOWN[#SHOWN + 1] = { id = id, who = who, text = text, kind = kind }; return orig(id, who, text, kind) end");
}

static void luaTarget(const char* kind, const char* name) {
  char code[200];
  if (!strcmp(kind, "dm")) snprintf(code, sizeof code, "STORE.otp_dk = 'dm'; STORE.otp_dm = '%s'; APP.on_open(320, 240)", name);
  else snprintf(code, sizeof code, "STORE.otp_dk = 'channel'; STORE.otp_ch = '%s'; APP.on_open(320, 240)", name);
  lrun(code);
}

// ---- running both --------------------------------------------------------------
static TestIo* g_io;
static otp::Core* g_core;

static void deliver() {
  while (!g_air.empty()) {
    Air a = g_air.front();
    g_air.pop_front();
    if (a.to_native) {
      if (a.dm) g_core->onDirect(kLuaPub, kLuaName, a.text.c_str());
      else g_core->onChannel(a.chan.c_str(), kLuaName, a.text.c_str());
    } else {
      lua_getglobal(L, "APP");
      lua_getfield(L, -1, "on_message");
      lua_newtable(L);
      lua_pushstring(L, a.dm ? "dm" : "channel"); lua_setfield(L, -2, "kind");
      lua_pushstring(L, a.dm ? "" : a.chan.c_str()); lua_setfield(L, -2, "channel");
      lua_pushstring(L, kNativeName); lua_setfield(L, -2, "sender");
      lua_pushstring(L, a.text.c_str()); lua_setfield(L, -2, "text");
      if (lua_pcall(L, 1, 0, 0) != LUA_OK) { fprintf(stderr, "on_message: %s\n", lua_tostring(L, -1)); lua_pop(L, 1); g_fail++; }
      lua_pop(L, 1);
    }
  }
}

static void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 100) {
    g_now += 100;
    lrun("APP.on_tick()");
    g_core->tick();
    deliver();
  }
}

static std::string randomHex(int n) {
  std::string s(n * 2 + 1, 0);
  std::vector<uint8_t> b(n);
  for (auto& x : b) x = (uint8_t)rnd();
  otp::toHex(b.data(), n, &s[0]);
  s.resize(n * 2);
  return s;
}

static const otp::Entry* lastEntry(otp::Target* t) {
  g_core->ensureLog(*t);
  return t->log_n ? &t->log[t->log_n - 1] : nullptr;
}

static void checkDmCounters(otp::Target* t) {
  char code[160];
  snprintf(code, sizeof code, "load_pad('dm:%s', 'dm')", hex4(kNativePub).c_str());
  const long long lmine = lnum((std::string(code) + ".ctr_mine").c_str());
  const long long ltheirs = lnum((std::string(code) + ".ctr_theirs").c_str());
  if (g_verbose) printf("  counters: lua mine=%lld theirs=%lld  native mine=%u theirs=%u\n", lmine, ltheirs, t->ctr_mine, t->ctr_theirs);
  CHECK(lmine == (long long)t->ctr_theirs);
  CHECK(ltheirs == (long long)t->ctr_mine);
}

int main(int argc, char** argv) {
  const char* root = argc > 1 ? argv[1] : ".";
  std::string app = std::string(root) + "/test/data/otp/OTP_3_RC1.lua";
  std::string mock = std::string(root) + "/test/otp/wada_mock.lua";

  // Lua's key sorts first: Lua is role A, native role B (and the reverse check below).
  for (int i = 0; i < 32; i++) { kLuaPub[i] = (uint8_t)(0x10 + i); kNativePub[i] = (uint8_t)(0xA0 + i); }

  // ---- unit checks ----
  {
    uint8_t d[32];
    otp::sha256((const uint8_t*)"abc", 3, d);
    char h[65]; otp::toHex(d, 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    // RFC 4231 test case 2
    otp::hmacSha256((const uint8_t*)"Jefe", 4, (const uint8_t*)"what do ya want for nothing?", 28, d);
    otp::toHex(d, 32, h);
    CHECK(!strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    char b[8]; otp::toBase36(0, b); CHECK(!strcmp(b, "0"));
    otp::toBase36(1295, b); CHECK(!strcmp(b, "zz"));
    uint32_t v; CHECK(otp::fromBase36("zz", 2, &v) && v == 1295);
    CHECK(otp::maxSingleShotBytes(6) == 73);
    CHECK(otp::maxChunkBytes(6) == 61);
    CHECK(otp::maxChannelBytes(6) == 65);
  }

  luaStart(app.c_str(), mock.c_str());
  TestIo io;
  otp::Core core(&io);
  g_io = &io;
  g_core = &core;
  core.begin();

  // ---- DM pad shared both ways ----
  const std::string pad = randomHex(3000);
  {
    char code[7000];
    snprintf(code, sizeof code, "assert(import_pad_hex('dm:%s', 'dm', '%s', resolve_dm_role({ pubkey = PEER_PK })))",
             hex4(kNativePub).c_str(), pad.c_str());
    lrun(code);
  }
  otp::Target* dm = core.dmTarget(kLuaPub, kLuaName);
  char err[160];
  CHECK(core.importHex(*dm, pad.data(), pad.size(), err, sizeof err));
  CHECK(dm->role == 'B');
  {
    char code[120];
    snprintf(code, sizeof code, "load_pad('dm:%s', 'dm').role", hex4(kNativePub).c_str());
    CHECK(lstr(code) == "A");
  }
  core.setOpenTarget(dm);

  // 1. Lua -> native, one part
  printf("DM lua -> native\n");
  lrun("send_chat_message('hello from the lua app')");
  run(3000);
  {
    const otp::Entry* e = lastEntry(dm);
    CHECK(e && !e->mine && std::string(e->text, e->len) == "hello from the lua app");
    bool delivered = false;
    lrun("DELIV = false; for _, t in ipairs(TOASTS) do if t == 'all parts delivered' then DELIV = true end end");
    delivered = lstr("DELIV") == "true";
    CHECK(delivered);
    checkDmCounters(dm);
  }

  // 2. native -> Lua, several parts
  printf("DM native -> lua (multi-part)\n");
  std::string longmsg;
  for (int i = 0; i < 6; i++) longmsg += "The quick brown fox jumps over the lazy dog. ";
  CHECK(core.send(*dm, longmsg.data(), longmsg.size(), err, sizeof err) == otp::SendResult::Ok);
  run(8000);
  {
    const otp::Entry* e = lastEntry(dm);
    CHECK(e && e->mine && e->state == otp::State::Delivered);
    CHECK(lstr("SHOWN[#SHOWN].text") == longmsg);
    CHECK(lstr("SHOWN[#SHOWN].kind") == "received");
    checkDmCounters(dm);
  }

  // 3. Lua -> native multi-part
  printf("DM lua -> native (multi-part)\n");
  {
    std::string code = "send_chat_message('" + longmsg + longmsg + "')";
    lrun(code.c_str());
  }
  run(12000);
  {
    const otp::Entry* e = lastEntry(dm);
    CHECK(e && !e->mine && std::string(e->text, e->len) == longmsg + longmsg);
    checkDmCounters(dm);
  }

  // 3b. plain DMs that merely look like hex are left alone (and cost no pad)
  printf("hex-looking plain text\n");
  {
    const uint32_t m0 = dm->ctr_mine, t0 = dm->ctr_theirs;
    const size_t n0 = dm->log_n;
    CHECK(!core.onDirect(kLuaPub, kLuaName, "cafe"));
    CHECK(!core.onDirect(kLuaPub, kLuaName, "1234"));
    CHECK(!core.onDirect(kLuaPub, kLuaName, "deadbeefdeadbeefdeadbeef"));   // 24 hex: long enough, not framed
    CHECK(!core.onDirect(kLuaPub, kLuaName, "8f2c4a90b1d3e5f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6e7f"));
    CHECK(!core.onDirect(kLuaPub, kLuaName, "z4 is a road"));
    CHECK(!core.onDirect(kLuaPub, kLuaName, "z3 at noon?"));
    CHECK(dm->ctr_mine == m0 && dm->ctr_theirs == t0 && dm->log_n == n0);
  }

  // 4. sync check from native
  printf("DM sync check\n");
  run(25000);   // the Lua app answers a sync check at most every 10 s (and the hex DMs above prompted one)
  core.requestSyncCheck(*dm);
  run(2000);
  CHECK(dm->sync_ok == 1);
  if (g_verbose) printf("  sync: %s\n", dm->sync_text);

  // ---- channel ----
  printf("channel both ways\n");
  const std::string cpad = randomHex(2000);
  {
    char code[5000];
    snprintf(code, sizeof code, "assert(import_pad_hex('ch:ops', 'channel', '%s'))", cpad.c_str());
    lrun(code);
  }
  otp::Target* ch = core.channelTarget("ops");
  CHECK(ch && core.importHex(*ch, cpad.data(), cpad.size(), err, sizeof err));
  core.setOpenTarget(ch);
  luaTarget("channel", "ops");
  lrun("send_chat_message('channel hello')");
  run(4000);
  {
    const otp::Entry* e = lastEntry(ch);
    CHECK(e && !e->mine && std::string(e->text, e->len) == "channel hello");
  }
  CHECK(core.send(*ch, "reply on the channel", 20, err, sizeof err) == otp::SendResult::Ok);
  run(6000);
  CHECK(lstr("SHOWN[#SHOWN].text") == "reply on the channel");
  CHECK(lnum("load_pad('ch:ops', 'channel').ctr") == (long long)ch->ctr);
  {
    // our own entry should be Sent with the fingerprint's repeats shown
    const otp::Entry* own = nullptr;
    for (int i = ch->log_n - 1; i >= 0; i--) if (ch->log[i].mine) { own = &ch->log[i]; break; }
    CHECK(own && own->state == otp::State::Sent && own->rep == 2);
  }

  {
    const uint32_t c0 = ch->ctr;
    CHECK(!core.onChannel("ops", kLuaName, "cafe"));
    CHECK(!core.onChannel("ops", kLuaName, "deadbeefdeadbeefdeadbeefdeadbeef"));
    CHECK(!core.onChannel("ops", kLuaName, "z2 good news"));
    CHECK(ch->ctr == c0);
  }

  // 5. a lost channel post: native must notice the jump and resync
  printf("channel loss + auto-resync\n");
  g_drop_lua_channel = 1;
  lrun("send_chat_message('this one is lost')");
  run(4000);
  lrun("send_chat_message('this one jumps ahead')");
  run(4000);
  // (native is now out of step and resyncing; timing decides which gap it is on)
  run(30000);   // two gaps: the lost post, then the one that arrived while out of step
  CHECK(!ch->pending_resync);
  CHECK(lnum("load_pad('ch:ops', 'channel').ctr") == (long long)ch->ctr);
  lrun("send_chat_message('after the resync')");
  run(4000);
  {
    const otp::Entry* e = lastEntry(ch);
    CHECK(e && std::string(e->text, e->len) == "after the resync");
  }

  // 6. mismatched pads: garbage -> nack G -> the Lua bubble says failed
  printf("mismatched channel pad -> nack\n");
  {
    const std::string a = randomHex(500), b = randomHex(500);
    char code[1400];
    snprintf(code, sizeof code, "assert(import_pad_hex('ch:bad', 'channel', '%s'))", a.c_str());
    lrun(code);
    otp::Target* bad = core.channelTarget("bad");
    CHECK(core.importHex(*bad, b.data(), b.size(), err, sizeof err));
    luaTarget("channel", "bad");
    lrun("send_chat_message('this text will not decrypt at the other end')");
    run(15000);
    const otp::Entry* e = lastEntry(bad);
    CHECK(e && e->state == otp::State::Lost);
    CHECK(lstr("(function() local l = chat_log_for('ch:bad'); return l[#l].kind end)()") == "failed");
    CHECK(lstr("(function() local l = chat_log_for('ch:bad'); return l[#l].fail end)()") == "G");
  }

  // 6b. moving the Lua app's pads (and their counters) over to the native app
  printf("import from the Lua app\n");
  {
    std::string sav = lstr("(function() local t = {} for k, v in pairs(STORE) do t[#t + 1] = k .. '=' .. tostring(v) end return table.concat(t, '\\n') end)()");
    otp::Core::LuaSource src;
    src.ctx = nullptr;
    src.readFile = [](void*, const char* name, Bytes& out) -> bool {
      lua_getglobal(L, "FILES");
      lua_getfield(L, -1, name);
      size_t n = 0;
      const char* v = lua_tolstring(L, -1, &n);
      const bool ok = v != nullptr;
      if (ok) out.assign(v, (uint32_t)n);
      lua_pop(L, 2);
      return ok;
    };
    src.findContact = [](void*, const char* hex8, uint8_t pub[32], char* name, size_t cap) -> bool {
      if (hex4(kNativePub) != hex8) return false;   // the Lua app's peer is the native node
      memcpy(pub, kNativePub, 32);
      snprintf(name, cap, "%s", kNativeName);
      return true;
    };
    src.findChannel = [](void*, const char* s, char* name, size_t cap) -> bool {
      snprintf(name, cap, "%s", s);
      return true;
    };
    TestIo iom;
    otp::Core cm(&iom);
    cm.begin();
    char e2[120];
    const int moved = cm.importLua(sav.data(), sav.size(), src, nullptr, e2, sizeof e2);
    if (g_verbose) printf("  moved %d pads\n", moved);
    CHECK(moved == 3);   // dm, #ops, #bad
    otp::Target* md = cm.dmTarget(kNativePub, kNativeName);
    char code[160];
    snprintf(code, sizeof code, "load_pad('dm:%s', 'dm')", hex4(kNativePub).c_str());
    CHECK((long long)md->ctr_mine == lnum((std::string(code) + ".ctr_mine").c_str()));
    CHECK((long long)md->ctr_theirs == lnum((std::string(code) + ".ctr_theirs").c_str()));
    CHECK(std::string(1, md->role) == lstr((std::string(code) + ".role").c_str()));
    CHECK(cm.available(*md) == (int)lnum(("emmb(" + std::string(code) + ")").c_str()));
    otp::Target* mc = cm.channelTarget("ops");
    CHECK((long long)mc->ctr == lnum("load_pad('ch:ops', 'channel').ctr"));
    // A second run moves nothing (pads already here).
    CHECK(cm.importLua(sav.data(), sav.size(), src, nullptr, e2, sizeof e2) == 0);
  }

  // 7. persistence + passphrase lock
  printf("persistence + lock\n");
  {
    const int avail_before = core.available(*dm);
    run(3000);   // flush logs
    CHECK(core.enableProtection("correct horse"));
    bool all_enc = true;
    for (auto& f : io.files) {
      if (f.first == "settings") continue;
      if (f.second.size() >= 4 && f.second.compare(0, 4, "ENC2") != 0) { all_enc = false; printf("  plain: %s\n", f.first.c_str()); }
    }
    CHECK(all_enc);
    TestIo io2;
    io2.files = io.files;
    otp::Core c2(&io2);
    c2.begin();
    CHECK(c2.locked());
    CHECK(!c2.unlock("wrong"));
    CHECK(c2.unlock("correct horse"));
    otp::Target* d2 = c2.dmTarget(kLuaPub, kLuaName);
    CHECK(c2.available(*d2) == avail_before);
    c2.ensureLog(*d2);
    CHECK(d2->log_n == dm->log_n);
    CHECK(d2->role == 'B');
  }

  // 8. roles the other way round: native sorts first
  printf("swapped roles\n");
  {
    lua_close(L);
    for (int i = 0; i < 32; i++) { kLuaPub[i] = (uint8_t)(0xC0 + i); kNativePub[i] = (uint8_t)(0x20 + i); }
    luaStart(app.c_str(), mock.c_str());
    TestIo io3;
    otp::Core c3(&io3);
    g_core = &c3;
    c3.begin();
    const std::string p3 = randomHex(1500);
    char code[3400];
    snprintf(code, sizeof code, "assert(import_pad_hex('dm:%s', 'dm', '%s', resolve_dm_role({ pubkey = PEER_PK })))",
             hex4(kNativePub).c_str(), p3.c_str());
    lrun(code);
    otp::Target* d3 = c3.dmTarget(kLuaPub, kLuaName);
    CHECK(c3.importHex(*d3, p3.data(), p3.size(), err, sizeof err));
    CHECK(d3->role == 'A');
    c3.setOpenTarget(d3);
    CHECK(c3.send(*d3, "native is A now", 15, err, sizeof err) == otp::SendResult::Ok);
    run(4000);
    CHECK(lstr("SHOWN[#SHOWN].text") == "native is A now");
    lrun("send_chat_message('and lua is B')");
    run(4000);
    const otp::Entry* e = lastEntry(d3);
    CHECK(e && std::string(e->text, e->len) == "and lua is B");
    checkDmCounters(d3);
    run(11000);
    c3.requestSyncCheck(*d3);
    run(2000);
    CHECK(d3->sync_ok == 1);
    // A passphrase-protected Lua app moves over only with its passphrase.
    lrun("finish_enable_protection('lua pass')");
    lrun("local id = 'dm:' .. PEER_PK; local r = load_pad(id, 'dm'); r._hex_dirty = true; save_pad(id, r)");
    CHECK(lstr("string.sub(FILES['otp3pad_dm_' .. PEER_PK .. '.hex'] or '', 1, 5)") == "ENC2:");
    std::string sav = lstr("(function() local t = {} for k, v in pairs(STORE) do t[#t + 1] = k .. '=' .. tostring(v) end return table.concat(t, '\\n') end)()");
    CHECK(otp::Core::luaNeedsPassphrase(sav.data(), sav.size()));
    otp::Core::LuaSource src;
    src.ctx = nullptr;
    src.readFile = [](void*, const char* name, Bytes& out) -> bool {
      lua_getglobal(L, "FILES");
      lua_getfield(L, -1, name);
      size_t n = 0;
      const char* v = lua_tolstring(L, -1, &n);
      const bool ok = v != nullptr;
      if (ok) out.assign(v, (uint32_t)n);
      lua_pop(L, 2);
      return ok;
    };
    src.findContact = [](void*, const char* hex8, uint8_t pub[32], char* name, size_t cap) -> bool {
      if (hex4(kNativePub) != hex8) return false;
      memcpy(pub, kNativePub, 32);
      snprintf(name, cap, "%s", kNativeName);
      return true;
    };
    src.findChannel = [](void*, const char*, char*, size_t) -> bool { return false; };
    TestIo iop;
    otp::Core cp(&iop);
    cp.begin();
    char e3[120];
    CHECK(cp.importLua(sav.data(), sav.size(), src, "nope", e3, sizeof e3) == -1);
    CHECK(cp.importLua(sav.data(), sav.size(), src, "lua pass", e3, sizeof e3) == 1);
    otp::Target* pd = cp.dmTarget(kNativePub, kNativeName);
    CHECK(cp.available(*pd) == c3.available(*d3));
    CHECK(pd->ctr_mine == d3->ctr_theirs && pd->ctr_theirs == d3->ctr_mine);
    g_core = &core;
  }

  lua_close(L);
  if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
  printf("all OTP interop checks passed\n");
  return 0;
}
