// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// OTP Messenger: one-time-pad messaging over MeshCore DMs and channels.
//
// A native port of Jade's OTP_3_RC1.lua (build 20261006.109-colors), kept
// byte-for-byte compatible on the air with that app and with MeshCore Open
// TAK's OTP mode:
//
//   DM       bare lowercase hex of (framed ^ pad). Sends are always framed as
//            "\1MSGCHNK\2"+tid+idx+tot+data and acked with "\1MSGACK\2"+tid+idx
//            (also encrypted). Unframed plaintext is accepted from senders
//            that send single-shot messages.
//   channel  "z2<offset b36>|<hex>", single packet, after a 0.6-2.2 s jitter.
//   sync     "z3" + s|S<ctr b36>  (channel)  or  p|P<role><mine b36>:<theirs b36>
//   nack     "z4" + C|D|G + "z"   (collided, decrypt error, garbled)
//
// DM pads are split by role: A sends from the front and receives from the
// back; B the opposite. The role comes from comparing the two public keys.
// Channel pads are one shared counter that every member advances.
//
// Platform-neutral and host-tested against the Lua app itself
// (test/test_otp_interop.cpp runs OTP_3_RC1.lua in-process). Everything runs
// on one thread (the Arduino loop on the device).

#include <stddef.h>
#include <stdint.h>

namespace otp {

// ---- limits (as OTP_3_RC1.lua) ----------------------------------------------
constexpr int kMaxTotalPadBytes = 8200;
constexpr int kMaxAddBytes = 8200;
constexpr int kChatHistoryMax = 150;
constexpr int kMaxTotalChatEntries = 500;
constexpr int kWaitingQueueCap = 5;
constexpr int kMaxTargets = 40;
constexpr int kLowPadBytes = 100;
constexpr int kMaxNameLen = 32;

// ---- memory -----------------------------------------------------------------
// Large buffers (pads, chat text) go through these, so the device can keep them
// in PSRAM. Defaults to malloc/free.
void setAllocator(void* (*alloc_fn)(size_t), void (*free_fn)(void*));
void* xalloc(size_t n);
void xfree(void* p);

struct Bytes {
  uint8_t* p = nullptr;
  uint32_t n = 0;
  uint32_t cap = 0;
  Bytes() {}
  ~Bytes() { release(); }
  Bytes(const Bytes&) = delete;
  Bytes& operator=(const Bytes&) = delete;
  Bytes(Bytes&& o) noexcept { swap(o); }
  Bytes& operator=(Bytes&& o) noexcept { if (this != &o) { release(); swap(o); } return *this; }
  bool reserve(uint32_t want);
  bool append(const void* data, uint32_t len);
  bool assign(const void* data, uint32_t len) { n = 0; return append(data, len); }
  void clear() { n = 0; }
  void release();
  void swap(Bytes& o);
};

// ---- crypto (portable, also used for the passphrase lock) --------------------
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void hmacSha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len,
                uint8_t out[32]);

// ---- small helpers (exposed for tests) ----------------------------------------
void toHex(const uint8_t* b, size_t n, char* out);   // lowercase, NUL-terminated (2n+1)
bool fromHex(const char* hex, size_t hex_len, Bytes& out);
void toBase36(uint32_t v, char* out);                 // out >= 8
bool fromBase36(const char* s, size_t len, uint32_t* out);
bool looksGarbled(const uint8_t* s, size_t n);        // OTP_3_RC1 llg()
int maxSingleShotBytes(int self_name_len);
int maxChunkBytes(int self_name_len);
int maxChannelBytes(int self_name_len);

enum class Kind : uint8_t { Dm = 0, Channel = 1 };

enum class State : uint8_t {
  Received = 0,
  Pending,     // being encrypted / waiting for the channel jitter / chunks going
  Sent,
  Retrying,
  Waiting,     // no path yet, queued (store-and-forward)
  Delivered,   // every chunk acked
  Failed,      // a nack came back (fail = C/D/G)
  Lost,        // "warn": garbage, collision, or a send that gave up
};

struct Entry {
  uint32_t seq = 0;
  uint32_t at_epoch = 0;   // 0: clock not set
  uint32_t at_ms = 0;
  char who[kMaxNameLen + 1] = {0};   // sender; "" for our own
  char* text = nullptr;
  uint16_t len = 0;
  State state = State::Received;
  char fail = 0;
  char fail_who[kMaxNameLen + 1] = {0};
  bool mine = false;
  bool retried = false;
  uint8_t rep = 0;
  uint32_t fp = 0;
  uint32_t rep_t0 = 0;
};

struct Participant {
  char name[kMaxNameLen + 1];
  uint32_t last_heard_ms;
  int8_t in_sync;   // -1 unknown
};

struct Imt;   // inbound chunk reassembly

struct WaitItem {
  char* cipher;    // xalloc'd wire text
  uint32_t entry_seq;
  uint32_t queued_ms;
  uint32_t last_probe_ms;
  bool probed;
};

struct Target {
  bool used = false;
  bool keep = false;                  // listed in the index (has, or had, a pad or messages)
  Kind kind = Kind::Dm;
  char id[48] = {0};                  // "dm:<8 hex>" or "ch:<name>"
  char name[kMaxNameLen + 1] = {0};   // contact name / channel name without '#'
  uint8_t pub[32] = {0};              // DM contact
  bool have_pub = false;

  // pad
  bool loaded = false;
  Bytes pad;
  char role = 'A';
  uint32_t ctr = 0, ctr_mine = 0, ctr_theirs = 0;
  bool read_failed = false;
  bool legacy = false;
  bool hex_dirty = false;
  bool save_pending = false;
  uint8_t save_retries = 0;
  uint32_t save_last_ms = 0;

  // sync
  int8_t sync_ok = -1;
  char sync_text[136] = {0};
  bool pending_resync = false;
  Kind pr_kind = Kind::Dm;
  uint32_t pr_peer_ctr = 0, pr_first_ms = 0;
  uint8_t resync_streak = 0;
  char badge[40] = {0};
  uint32_t badge_until = 0;
  bool sync_backoff = false;
  uint32_t awaiting_since = 0;
  uint32_t next_sync_check = 0;
  uint32_t last_sync_sent = 0;
  bool sent_sync_once = false;

  Imt* imt = nullptr;
  WaitItem wq[kWaitingQueueCap] = {};
  uint8_t wq_n = 0;

  Participant parts[8] = {};
  uint8_t parts_n = 0;

  Entry* log = nullptr;
  uint16_t log_n = 0, log_cap = 0;
  uint32_t touched_ms = 0;
  uint16_t unread = 0;
  bool log_loaded = false;
  bool log_dirty = false;
};

// The host side: radio, clock, randomness, storage, UI.
class Io {
 public:
  virtual ~Io() {}
  virtual uint32_t nowMs() = 0;
  virtual uint32_t epoch() = 0;   // 0: unknown
  virtual uint8_t randomByte() = 0;
  virtual bool airtimeClear() = 0;
  virtual const char* selfName() = 0;
  virtual bool selfPub(uint8_t out[32]) = 0;
  // DM by public key; channel by name (without '#'). `fp` is the flood's
  // fingerprint for repeatsFor().
  virtual bool sendDm(const uint8_t pub[32], const char* text) = 0;
  virtual bool sendChannel(const char* name, const char* text, uint32_t* fp) = 0;
  virtual uint8_t repeatsFor(uint32_t fp) = 0;
  // Storage: flat names, no directories.
  virtual bool readFile(const char* name, Bytes& out) = 0;
  virtual bool writeFile(const char* name, const uint8_t* data, uint32_t len) = 0;
  virtual bool removeFile(const char* name) = 0;
  virtual void removeAll() = 0;   // panic wipe: every file of this app
  // UI
  virtual void toast(const char* msg) = 0;
  virtual void changed(Target& t) = 0;                       // anything on screen may differ
  virtual void incoming(Target& t, const Entry& e) = 0;      // new message for the user
};

enum class SendResult : uint8_t { Ok, NoTarget, Public, Busy, QueueFull, TooLong, NoPad, Error };

class Core {
 public:
  explicit Core(Io* io) : io_(io) {}
  ~Core();

  // ---- lifecycle / lock ----
  void begin();                                   // load settings; pads load lazily
  bool protectionEnabled() const { return prot_salt_[0] != 0; }
  bool locked() const { return protectionEnabled() && !have_key_; }
  bool unlock(const char* passphrase);            // false: wrong passphrase
  bool enableProtection(const char* passphrase);  // re-saves every pad encrypted
  void lockNow();                                 // forget the key (pads stay on disk)
  void panicWipe();                               // every pad, log and the lock

  // ---- targets ----
  Target* dmTarget(const uint8_t pub[32], const char* name);   // created on demand
  Target* channelTarget(const char* name);                      // nullptr for Public
  Target* findById(const char* id);
  int targetCount() const;
  Target* targetAt(int i);
  static bool isPublicChannel(const char* name);

  // ---- pad ----
  int available(Target& t);                       // emmb()
  bool hasPad(Target& t) { return available(t) > 0 || t.ctr || t.ctr_mine || t.ctr_theirs; }
  bool importHex(Target& t, const char* hex, size_t len, char* err, size_t err_cap);
  bool addBytes(Target& t, const uint8_t* data, size_t len, char* err, size_t err_cap);
  int generate(Target& t, int n);                 // returns bytes added
  void wipe(Target& t);
  void flipRole(Target& t);
  char autoRole(Target& t);                       // 0 when unknown
  bool exportHex(Target& t, Bytes& out);          // the UNUSED part of the pad, as the peer imports it

  // ---- messaging ----
  SendResult send(Target& t, const char* text, size_t len, char* err, size_t err_cap);
  bool busy() const;                              // one send at a time, as the Lua app
  bool cancelWaiting(Target& t);
  void requestSyncCheck(Target& t);               // the "Check Sync" / ReSync button
  void setOpenTarget(Target* t);                  // the chat on screen (auto sync checks)
  Target* openTarget() const { return open_; }
  void markRead(Target& t);
  int unreadTotal();
  void forget(Target& t);                         // delete the pad, its history and the entry

  // Incoming text. Returns true when it was OTP traffic for a target with a
  // pad (so the normal chat should not show it).
  bool onDirect(const uint8_t sender_pub[32], const char* sender_name, const char* text);
  bool onChannel(const char* channel_name, const char* sender, const char* body);

  void tick();

  // ---- view helpers ----
  const char* statusWord(Target& t, const Entry& e, char* buf, size_t cap);
  int sendProgress(Target& t, int* total);        // chunks acked so far, -1 if none
  int recvProgress(Target& t, int* total);
  int activeParticipants(Target& t, Participant* out, int max);
  void ensureLog(Target& t);

  // settings (persisted)
  uint8_t theme = 0;          // 0..5
  bool sound = true;
  bool show_participants = false;
  bool keep_files = true;
  void saveSettings();

  static constexpr const char* kBuildId = "20261012.native-1";

 private:
  friend struct CoreTest;
  Io* io_;
  Target targets_[kMaxTargets];
  Target* open_ = nullptr;
  uint32_t seq_ = 1;
  int total_entries_ = 0;

  // protection
  char prot_salt_[33] = {0};
  char prot_check_[65] = {0};
  uint8_t key_[32] = {0};
  bool have_key_ = false;

  // sending (one at a time)
  struct Psend {
    bool active = false;
    Target* t = nullptr;
    Bytes text;
    int ccap = 0;
    uint8_t tid = 0, tot = 0, sctr = 0;
    char* cur_cipher = nullptr;
    int cur_idx = -1;
    uint32_t last_sent = 0;
    bool sent_once = false;
    uint16_t retries = 0;
    uint32_t entry_seq = 0;
  } psend_;
  struct Ssend {
    bool active = false;
    Target* t = nullptr;
    char* cipher = nullptr;
    bool has_offset = false;
    uint32_t offset = 0;
    uint8_t retries = 0;
    uint32_t last_sent = 0;
    uint32_t entry_seq = 0;
  } ssend_;
  struct Queued {
    bool active = false;
    Target* t = nullptr;
    char* cipher = nullptr;
    bool has_offset = false;
    uint32_t offset = 0;
    uint32_t entry_seq = 0;
  } queued_;
  struct Jitter {
    bool active = false;
    Target* t = nullptr;
    char* text = nullptr;
    uint16_t len = 0;
    uint32_t fire_at = 0;
    uint32_t entry_seq = 0;
  } jitter_;

  struct Nack { char payload[8]; Target* t; char who[kMaxNameLen + 1]; uint8_t tries; };
  Nack nacks_[2];
  int nack_n_ = 0;
  uint32_t last_nack_ms_ = 0;

  struct AckCache { char key[64]; char* cipher; };
  AckCache acks_[24] = {};
  int ack_next_ = 0;

  uint64_t recent_wire_[6] = {0};
  int recent_wire_n_ = 0;
  struct Rmc { uint64_t key; uint32_t at; bool used; };
  Rmc rmc_[8] = {};
  int rmc_next_ = 0;
  uint32_t last_rep_ms_ = 0;
  uint32_t last_prune_ms_ = 0;

  // locked: incoming queued until unlock
  struct Held { bool dm; uint8_t pub[32]; char a[kMaxNameLen + 1]; char b[kMaxNameLen + 1]; char* text; };
  Held held_[12] = {};
  int held_n_ = 0;

  // internals
  Target* alloc(Kind k, const char* id);
  void loadPad(Target& t);
  bool persistPad(Target& t);
  void savePad(Target& t);
  bool consume(Target& t, uint32_t n, bool mine, char* err, size_t err_cap);
  bool keySlice(Target& t, uint32_t n, bool mine, uint8_t* out);
  bool reversed(const Target& t, bool mine) const;
  void serializeMeta(Target& t, char* out, size_t cap);
  void clearSyncState(Target& t);

  bool rawTransmit(Target& t, const char* wire, bool with_offset, uint32_t offset, uint32_t* fp,
                   const char* dm_override_name = nullptr);
  void sendSyncCheck(Target& t, bool is_reply);
  void handleSync(Target& t, const char* text, const char* sender);
  void handleNack(Target& t, const char* text, const char* who);
  void queueNack(Target& t, char reason, const char* who);
  bool sendAck(Target& t, uint8_t tid, uint8_t idx);
  void process(Target& t, const char* sender, const char* text, bool has_off, uint32_t off);
  bool finishReassembly(Target& t, const char* sender);
  bool sendNextChunk();
  void completeSend();
  void finishSingle(Target& t, char* cipher, bool has_off, uint32_t off, uint32_t entry_seq);
  bool waitingEnqueue(Target& t, char* cipher, uint32_t entry_seq);
  void flushWaiting(Target& t);
  void recordParticipant(Target& t, const char* sender, bool has_off, uint32_t off);

  void psendTick();
  void ssendTick();
  void jitterTick();
  void flushQueued();
  void nackTick();
  void repeatTick();
  void autoSyncTick(Target& t);
  void maybeAutoSyncCheck();
  void backoffTick();
  void waitingTick();
  void saveTick();
  void heldReplay();

  // chat log
  Entry* addEntry(Target& t, const char* who, const uint8_t* text, size_t len, State st, bool mine);
  Entry* entryBySeq(Target& t, uint32_t seq);
  void setState(Target& t, uint32_t seq, State st);
  void pruneTotal(Target* protect);
  void saveLog(Target& t);
  void loadLog(Target& t);
  void freeLog(Target& t);
  void requestPrompt(Target& t);
  void showBadge(Target& t, uint32_t skipped);
  void notifyChanged(Target& t) { io_->changed(t); }

  // at-rest encryption (ENC2, as the Lua app's enc_str/dec_str)
  bool encode(const uint8_t* in, uint32_t n, Bytes& out);
  bool decode(const uint8_t* in, uint32_t n, Bytes& out);
  bool deriveKey(const char* pass, const char* salt_hex, uint8_t out[32]);
  void loadSettings();
  void resaveAll();
  void freeTarget(Target& t);
  void markKeep(Target& t);
 public:
  void loadIndex();
 private:
  bool isDupWire(uint64_t h);
};

}  // namespace otp
