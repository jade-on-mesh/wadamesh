local app = {}
local BUILD_ID = "20261006.109-colors"
local PAL = {
encrypt    = 0xFF6B4A,
decrypt    = 0x2DD4BF,
ciphertext = 0xF97316,
plaintext  = 0x38BDF8,
newpad     = 0xA78BFA,
channel    = 0xEC4899,
result_ok  = 0x4ADE80,
keyphrase  = 0xFBBF24,
padinfo    = 0x22D3EE,
ready      = 0x94A3B8,
bg         = 0x000000,
}
local THNAMES, THEME_DATA = { "Default", "Cyberpunk", "Trans", "Cascadia", "B&W", "Acid" }, {
"FF6B4A2DD4BFF9731638BDF8A78BFAEC48994ADE80FBBF2422D3EE94A3B8000000",
"FF149339FF14BC13FE00F5FFFF00FFF5FF0039FF14FFAA0000FFEF6B7280000000",
"F5A9B85BCEFAF5A9B8FFFFFFA6E8FFFF69B45BCEFAF5A9B838BDF8E5E7EB000000",
"60A5FA38BDF8A78BFAFFFFFFFBBF2415803D84CC16F472B622C55ECBD5E1000000",
"FFFFFFE6E6E6CCCCCCFFFFFFB3B3B3D9D9D9FFFFFF9999998080804D4D4D000000",
"FF4500FFA5002F4F4FFFFFFF4682B4FF0000FFD700FFDAB98B0000696969000000",
}
local thmidx, PALETTE_FIELDS = 1, { "encrypt", "decrypt", "ciphertext", "plaintext", "newpad", "channel", "result_ok", "keyphrase", "padinfo", "ready", "bg" }
function apply_theme(idx)
local s = THEME_DATA[idx]
for i = 1, 11 do
PAL[PALETTE_FIELDS[i]] = tonumber(s:sub((i - 1) * 6 + 1, i * 6), 16)
end
end
local screens = {}
local screen_w, screen_h = 0, 0
local current_screen = nil
local current_back_target = nil
local W = {}
function register_screen(name, build_fn)
screens[name] = build_fn
end
local list_state = {}
function make_tracked_list(screen_name, x, y, w, h)
local list = wada.ui.list(x, y, w, h)
local prev = list_state[screen_name]
local state = { widget = list, count = 0, callbacks = {}, selected_idx = (prev and prev.selected_idx) or 1, is_real_list = true }
list_state[screen_name] = state
local api = {}
function api.add(label, fn)
list:add(label, fn)
state.count = state.count + 1
state.callbacks[state.count] = fn or function() end
return state.count
end
function api.finish()
if state.selected_idx > state.count then state.selected_idx = state.count end
if state.selected_idx < 1 then state.selected_idx = 1 end
if state.count > 0 then
pcall(function() list:select(state.selected_idx) end)
end
end
function api.color(i, c)
if type(list.color) == "function" then
pcall(function() list:color(i, c) end)
end
end
function api.selected()
local ok, idx = pcall(function() return list:selected() end)
if ok and type(idx) == "number" then return idx end
return state.selected_idx
end
return api
end
function keynav_start(screen_name)
local prev = list_state[screen_name]
local sel = (prev and prev.selected_idx) or 1
if sel < 1 then sel = 1 end
local state = {
widget = { select = function(self, idx) goto_screen(screen_name) end },
count = 0,
callbacks = {},
selected_idx = sel,
is_real_list = false,
}
list_state[screen_name] = state
return state
end
function keynav_button(state, label, x, y, w, h, fn)
state.count = state.count + 1
local display_label = (state.count == state.selected_idx) and ("> " .. label) or label
ui_button(display_label, x, y, w, h, fn)
state.callbacks[state.count] = fn
end
function keynav_finish(state)
if state.selected_idx > state.count then state.selected_idx = math.max(1, state.count) end
if state.selected_idx < 1 then state.selected_idx = 1 end
end
local SCREEN_REBUILD_DEBOUNCE_MS = 120
local screen_last_rebuild_ms = {}
local screen_pending_rebuild = nil
function do_goto_screen(name)
pcall(trim_pad_cache)
if wada.ui.clear then wada.ui.clear() end
W = {}
current_screen = name
current_back_target = nil
if wada.ui.scroll then wada.ui.scroll(true) end
local ok, err = pcall(screens[name], screen_w, screen_h)
if not ok then
wada.sys.toast("screen build error: " .. tostring(err))
end
end
function goto_screen(name)
if not screens[name] then
wada.sys.toast("no such screen: " .. tostring(name))
return
end
if name == current_screen then
local now = wada.sys.millis()
local last = screen_last_rebuild_ms[name]
if last and (now - last) < SCREEN_REBUILD_DEBOUNCE_MS then
screen_pending_rebuild = name
return
end
end
screen_last_rebuild_ms[name] = wada.sys.millis()
screen_pending_rebuild = nil
do_goto_screen(name)
end
function screen_rebuild_tick()
if not screen_pending_rebuild then return end
local now = wada.sys.millis()
local last = screen_last_rebuild_ms[screen_pending_rebuild]
if last and (now - last) < SCREEN_REBUILD_DEBOUNCE_MS then return end
local pending = screen_pending_rebuild
screen_pending_rebuild = nil
screen_last_rebuild_ms[pending] = now
do_goto_screen(pending)
end
local PAGE_CYCLE = { "home", "chat", "contacts", "pad", "settings", "channels" }
function page_cycle_index(name)
for i, nm in ipairs(PAGE_CYCLE) do
if nm == name then return i end
end
return 1
end
function page_next()
local i = page_cycle_index(current_screen)
goto_screen(PAGE_CYCLE[(i % #PAGE_CYCLE) + 1])
end
function page_prev()
local i = page_cycle_index(current_screen)
goto_screen(PAGE_CYCLE[((i - 2) % #PAGE_CYCLE) + 1])
end
function nav_arrows(x, y, w, h, back_target)
if not back_target then
local arrow_w = math.floor((w - 4) / 2)
ui_button("<", x, y, arrow_w, h, safe(page_prev))
ui_button(">", x + arrow_w + 4, y, arrow_w, h, safe(page_next))
return
end
current_back_target = back_target
local arrow_w = 30
local mid_w = w - 2 * (arrow_w + 3)
ui_button("<", x, y, arrow_w, h, safe(page_prev))
ui_button("Back", x + arrow_w + 3, y, mid_w, h, safe(function() goto_screen(back_target) end))
ui_button(">", x + arrow_w + 3 + mid_w + 3, y, arrow_w, h, safe(page_next))
end
local kb_active = false
local kb_buffer = ""
local kb_title = ""
local kb_callback = nil
local kb_preview = nil
local kb_submit_label = "Send"
local kb_show_pad_counter = false
local kb_counter_label = nil
local kb_counter_fn = nil
local chat_compose_active = false
local compose_buffer = ""
function text_input(title, initial, cb, submit_label, show_pad_counter, counter_fn)
if type(wada.ui.input) == "function" and not submit_label and not show_pad_counter then
wada.ui.input(title, initial or "", cb)
return
end
kb_active = true
kb_buffer = initial or ""
kb_title = title
kb_callback = cb
kb_submit_label = submit_label or "Send"
kb_show_pad_counter = show_pad_counter or false
kb_counter_fn = counter_fn
W = {}
kb_render()
end
function kb_render()
if wada.ui.clear then wada.ui.clear() end
ui_label(kb_title, 2, 2, 14, wada.ui.colors.text)
local preview_w = screen_w - 4
local preview_y = 24
local line_h = (wada.ui.text_h and pcall(wada.ui.text_h, 12) and wada.ui.text_h(12)) or 14
local safe_bottom = (kb_show_pad_counter and (screen_h - 28 - 16)) or (screen_h - 28)
local max_lines = math.max(1, math.floor((safe_bottom - preview_y) / line_h))
local raw_text = kb_buffer == "" and "(empty)" or kb_buffer
local shown_text = raw_text
if type(wada.ui.text_lines) == "function" then
local ok_lines, lines = pcall(wada.ui.text_lines, raw_text, preview_w, 12)
if ok_lines and type(lines) == "number" and lines > max_lines then
local lo, hi = 1, #raw_text
while lo < hi do
local mid = math.floor((lo + hi) / 2)
local candidate = raw_text:sub(mid)
local ok2, l2 = pcall(wada.ui.text_lines, candidate, preview_w, 12)
if ok2 and type(l2) == "number" and l2 <= max_lines then
hi = mid
else
lo = mid + 1
end
end
shown_text = raw_text:sub(lo)
end
end
kb_preview = ui_label(shown_text, 2, preview_y, 12, wada.ui.colors.sub)
if kb_preview.width then kb_preview:width(preview_w) end
if kb_show_pad_counter then
local counter_y = preview_y + max_lines * line_h + 4
kb_counter_label = ui_label("", 2, counter_y, 11, PAL.padinfo)
refresh_kb_counter()
end
ui_button("Cancel", 2, screen_h - 28, screen_w - 4, 24, safe(kb_cancel))
end
function refresh_kb_counter()
if not kb_show_pad_counter or not kb_counter_label or not kb_counter_fn then return end
local avail = kb_counter_fn()
local remaining = avail - #kb_buffer
local label_text = string.format("%d typed / %d bytes remaining after send", #kb_buffer, remaining)
kb_counter_label:set(label_text)
if kb_counter_label.color then
kb_counter_label:color(remaining < 0 and PAL.encrypt or PAL.padinfo)
end
end
function kb_submit()
local cb, text = kb_callback, kb_buffer
kb_active, kb_buffer, kb_callback, kb_preview = false, "", nil, nil
if cb then cb(text) end
end
function kb_cancel()
local cb = kb_callback
kb_active, kb_buffer, kb_callback, kb_preview = false, "", nil, nil
if cb then cb(nil) end
end
function kb_handle_input(ev)
if not kb_active then return false end
if (ev.type == "dir" or ev.type == "swipe") and ev.dir == "select" then
kb_submit()
return true
end
if ev.type ~= "key" then return false end
if ev.key == "enter" then
kb_submit()
elseif ev.key == "esc" then
kb_cancel()
elseif ev.key == "backspace" then
kb_buffer = kb_buffer:sub(1, #kb_buffer - 1)
if kb_preview then kb_preview:set(kb_buffer == "" and "(empty)" or kb_buffer) end
refresh_kb_counter()
elseif type(ev.key) == "string" and #ev.key == 1 then
kb_buffer = kb_buffer .. ev.key
if kb_preview then kb_preview:set(kb_buffer) end
refresh_kb_counter()
end
return true
end
local function raw_random_byte()
local r = wada.sys.random(256)
if type(r) ~= "number" then return 0 end
return math.floor(r) % 256
end
local entropy_acc = 0
function rand_byte()
local mixed = raw_random_byte() ~ raw_random_byte() ~ raw_random_byte() ~ raw_random_byte()
local ok_t, t = pcall(wada.sys.millis)
if ok_t and type(t) == "number" then
mixed = mixed ~ (math.floor(t) % 256)
end
local ok_s, stats = pcall(wada.mesh.stats)
if ok_s and type(stats) == "table" then
if type(stats.rssi) == "number" then
mixed = mixed ~ (math.floor(math.abs(stats.rssi)) % 256)
end
if type(stats.noise) == "number" then
mixed = mixed ~ (math.floor(math.abs(stats.noise)) % 256)
end
end
entropy_acc = (entropy_acc + mixed + 1) % 256
mixed = mixed ~ entropy_acc
return mixed
end
local HEXB = {}
for i = 0, 255 do HEXB[i] = string.format("%02x", i) end
local BATCH = 32
function b2h(bytes)
if wada.crypto and type(wada.crypto.hex) == "function" then
local ok, hex = pcall(wada.crypto.hex, bytes)
if ok and type(hex) == "string" then return hex end
end
local out, oi = {}, 0
local n = #bytes
local i = 1
while i <= n do
local j = math.min(i + BATCH - 1, n)
local parts = {}
for k = i, j do
parts[#parts + 1] = HEXB[string.byte(bytes, k)]
end
oi = oi + 1
out[oi] = table.concat(parts)
i = j + 1
end
return table.concat(out)
end
function h2b(hex)
hex = hex:gsub("%s+", "")
if #hex % 2 ~= 0 then
return nil, "hex odd"
end
local out, oi = {}, 0
local n = #hex
local group = BATCH * 2
local i = 1
while i <= n do
local j = math.min(i + group - 1, n)
local parts = {}
for k = i, j, 2 do
local byte = tonumber(hex:sub(k, k + 1), 16)
if not byte then
return nil, "bad hex@" .. k
end
parts[#parts + 1] = string.char(byte)
end
oi = oi + 1
out[oi] = table.concat(parts)
i = j + 1
end
return table.concat(out)
end
function xor_bytes(a, b)
local out, oi = {}, 0
local n = #a
local i = 1
while i <= n do
local j = math.min(i + BATCH - 1, n)
local parts = {}
for k = i, j do
parts[#parts + 1] = string.char(string.byte(a, k) ~ string.byte(b, k))
end
oi = oi + 1
out[oi] = table.concat(parts)
i = j + 1
end
return table.concat(out)
end
local KDF_ROUNDS, session_key = 3000, nil
function crypto_hmac_available()
return wada.crypto and wada.crypto.hmac_sha256 and wada.crypto.hex
end
function derive_key(passphrase, salt_hex)
if not crypto_hmac_available() then return nil, "no crypto" end
local ok_salt, salt = pcall(h2b, salt_hex)
if not ok_salt or not salt then return nil, "bad salt" end
local ok, key = pcall(function()
local k = salt
for i = 1, KDF_ROUNDS + 1 do
k = wada.crypto.hmac_sha256(k, passphrase)
end
return k
end)
if not ok or type(key) ~= "string" then return nil, "hmac failed" end
return key
end
function xor_keystream(key, nonce_or_data, maybe_data)
local nonce, data
if maybe_data ~= nil then nonce, data = nonce_or_data, maybe_data
else nonce, data = "", nonce_or_data end
local ok, result = pcall(function()
local out, total, counter, oi = {}, 0, 0, 0
local length = #data
while total < length do
local ctr_bytes = string.char(
(counter >> 24) & 0xFF, (counter >> 16) & 0xFF,
(counter >> 8) & 0xFF, counter & 0xFF)
local block = wada.crypto.hmac_sha256(key, nonce .. ctr_bytes)
if type(block) ~= "string" or #block == 0 then break end
local take = math.min(#block, length - total)
local parts = {}
for i = 1, take do
parts[i] = string.char(string.byte(data, total + i) ~ string.byte(block, i))
end
oi = oi + 1
out[oi] = table.concat(parts)
total = total + take
counter = counter + 1
end
return total >= length and table.concat(out) or nil
end)
if not ok or type(result) ~= "string" then return nil end
return result
end
local ENC_PREFIX = "ENC1:"
local ENC2_PREFIX = "ENC2:"
local ENC2_NONCE_BYTES = 8
local ENC2_TAG_BYTES = 16
local function enc2_subkeys()
local ok, enc_key = pcall(wada.crypto.hmac_sha256, session_key, "otp3-enc")
local ok2, mac_key = pcall(wada.crypto.hmac_sha256, session_key, "otp3-mac")
if not ok or not ok2 or type(enc_key) ~= "string" or type(mac_key) ~= "string" then return nil, nil end
return enc_key, mac_key
end
local function tags_equal(a, b)
if type(a) ~= "string" or type(b) ~= "string" or #a ~= #b then return false end
local diff = 0
for i = 1, #a do
diff = diff | (string.byte(a, i) ~ string.byte(b, i))
end
return diff == 0
end
function enc_str(plaintext)
if not session_key or plaintext == "" then return plaintext end
local ok, result = pcall(function()
local enc_key, mac_key = enc2_subkeys()
if not enc_key then return nil end
local nonce_bytes = {}
for i = 1, ENC2_NONCE_BYTES do nonce_bytes[i] = string.char(rand_byte()) end
local nonce = table.concat(nonce_bytes)
local xored = xor_keystream(enc_key, nonce, plaintext)
if not xored then return nil end
local tag = wada.crypto.hmac_sha256(mac_key, nonce .. xored)
if type(tag) ~= "string" then return nil end
tag = tag:sub(1, ENC2_TAG_BYTES)
return ENC2_PREFIX .. b2h(nonce .. tag .. xored)
end)
if not ok or not result then return plaintext end
return result
end
function dec_str(value)
if not session_key then return value end
if type(value) ~= "string" then return value end
if value:sub(1, #ENC2_PREFIX) == ENC2_PREFIX then
local ok, result = pcall(function()
local raw = h2b(value:sub(#ENC2_PREFIX + 1))
if not raw or #raw < ENC2_NONCE_BYTES + ENC2_TAG_BYTES then return nil end
local nonce = raw:sub(1, ENC2_NONCE_BYTES)
local tag = raw:sub(ENC2_NONCE_BYTES + 1, ENC2_NONCE_BYTES + ENC2_TAG_BYTES)
local xored = raw:sub(ENC2_NONCE_BYTES + ENC2_TAG_BYTES + 1)
local enc_key, mac_key = enc2_subkeys()
if not enc_key then return nil end
local expected = wada.crypto.hmac_sha256(mac_key, nonce .. xored)
if type(expected) ~= "string" or not tags_equal(expected:sub(1, ENC2_TAG_BYTES), tag) then return nil end
return xor_keystream(enc_key, nonce, xored)
end)
if not ok or not result then return "" end
return result
end
if value:sub(1, #ENC_PREFIX) == ENC_PREFIX then
local hex = value:sub(#ENC_PREFIX + 1)
local ok, raw = pcall(h2b, hex)
if not ok or not raw then return "" end
local ok2, result = pcall(xor_keystream, session_key, raw)
if not ok2 then return "" end
return result or ""
end
return value
end
function enc_pad(hexstr)
if not session_key or hexstr == "" then return hexstr end
local ok, raw = pcall(h2b, hexstr)
if not ok or not raw then return hexstr end
local encoded = enc_str(raw)
return encoded
end
function dec_pad(encoded)
if not session_key or type(encoded) ~= "string" or encoded == "" then return encoded end
if encoded:sub(1, #ENC2_PREFIX) ~= ENC2_PREFIX and encoded:sub(1, #ENC_PREFIX) ~= ENC_PREFIX then return encoded end
local raw = dec_str(encoded)
if raw == "" then return "" end
local ok, hex_out = pcall(b2h, raw)
if not ok then return "" end
return hex_out
end
function check_tag(key)
if not key then return nil end
local ok, digest = pcall(wada.crypto.hmac_sha256, key, "wadamesh-check-v1")
if not ok or type(digest) ~= "string" then return nil end
return b2h(digest)
end
function clean_err(err)
local s = tostring(err)
return s:gsub("^%[string [^%]]*%]:%d+:%s*", "")
end
function safe(fn)
return function(...)
local ok, err = pcall(fn, ...)
if not ok then
wada.sys.toast("error: " .. clean_err(err))
end
end
end

local NOOP_WIDGET = setmetatable({}, { __index = function() return function() end end })
function ui_label(...)
local ok, w = pcall(wada.ui.label, ...)
if ok and w then return w end
return NOOP_WIDGET
end
function ui_button(...)
local ok, w = pcall(wada.ui.button, ...)
if ok and w then return w end
return NOOP_WIDGET
end
function mt_fill(t) for i = 1, 1500 do t[i] = string.rep("x", 100) .. i end end
function mt_lua()
local t = {}
pcall(mt_fill, t)
return #t
end
function sget(key, default)
local raw = wada.store.get and wada.store.get(key)
if type(raw) ~= "string" then return default end
return dec_str(raw)
end
function sset(key, value)
wada.store.set(key, enc_str(tostring(value)))
end
function sget_plain(key, default)
local raw = wada.store.get and wada.store.get(key, default) or default
if raw == nil then return default end
if type(raw) ~= "string" and type(raw) ~= "number" then return default end
return raw
end
function sset_plain(key, value)
wada.store.set(key, value)
end
local PROT_SALT_KEY, PROT_CHECK_KEY = "otp3_prot_salt", "otp3_prot_check"
function protection_enabled()
local salt = wada.store.get and wada.store.get(PROT_SALT_KEY)
if type(salt) ~= "string" then salt = "" end
return #salt > 0
end
function gen_salt_hex()
local out = {}
for i = 1, 16 do
local b = rand_byte()
b = b ~ rand_byte()
b = b ~ (math.floor(wada.sys.millis() + i) % 256)
out[i] = string.format("%02x", b)
end
return table.concat(out)
end
function otp_encrypt(plaintext, key_hex)
local key_bytes, herr = h2b(key_hex)
if not key_bytes then return nil, "bad key slice: " .. tostring(herr) end
if #key_bytes < #plaintext then return nil, "key short " .. #plaintext .. "/" .. #key_bytes end
return b2h(xor_bytes(plaintext, key_bytes:sub(1, #plaintext)))
end
function otp_decrypt(cipher_hex, key_hex)
local cbytes, herr1 = h2b(cipher_hex)
if not cbytes then return nil, "bad ciphertext: " .. tostring(herr1) end
local kbytes, herr2 = h2b(key_hex)
if not kbytes then return nil, "bad key slice: " .. tostring(herr2) end
if #kbytes < #cbytes then return nil, "key short " .. #cbytes .. "/" .. #kbytes end
return xor_bytes(cbytes, kbytes:sub(1, #cbytes))
end
local MARKER = {
msg_chunk = "\1MSGCHNK\2",
msg_ack = "\1MSGACK\2",
}

local SYNC_MARKER = "z3"
local CHANNEL_OFFSET_MARKER = "z2"
local B36_ALPHABET = "0123456789abcdefghijklmnopqrstuvwxyz"
function to_base36(n)
n = math.floor(n or 0)
if n <= 0 then return "0" end
local out = {}
while n > 0 do
local d = n % 36
table.insert(out, 1, B36_ALPHABET:sub(d + 1, d + 1))
n = (n - d) / 36
end
return table.concat(out)
end
function from_base36(s)
if type(s) ~= "string" or s == "" then return nil end
local n = 0
for i = 1, #s do
local c = s:sub(i, i):lower()
local d = B36_ALPHABET:find(c, 1, true)
if not d then return nil end
n = n * 36 + (d - 1)
end
return n
end
local STORE_SAFE_HEX_CHARS = 1600
local MXADD = 8200
local MAX_TOTAL_PAD_BYTES = 8200
function target_id(kind, key)
if kind == "channel" then
return "ch:" .. normalize_channel(key)
end

return "dm:" .. tostring(key):lower()
end
function sanitize_id(id)
return (id:gsub("[^%w]", "_"))
end
function normalize_channel(s)
if s == nil then return "" end
return (tostring(s):gsub("^#+", ""))
end
function is_public_channel_name(name)
return normalize_channel(name):lower() == "public"
end
function is_public_channel_id(id)
if type(id) ~= "string" then return false end
local name = id:match("^ch:(.*)$")
if not name then return false end
return name:lower() == "public"
end

local pads_cache = {}
local pads_cache_mru = {}

local PADS_CACHE_MAX = 2
local known_pad_ids = {}

local PAD_SAVE_RETRY_BACKOFF_MS = 1800
local PAD_SAVE_MAX_RETRIES = 6
local pending_pad_saves = {}
function pad_kind_from_id(id)
if type(id) ~= "string" then return nil end
if id:sub(1, 3) == "ch:" then return "channel" end
if id:sub(1, 3) == "dm:" then return "dm" end
return nil
end
local function pads_cache_touch(id)
for i = #pads_cache_mru, 1, -1 do
if pads_cache_mru[i] == id then table.remove(pads_cache_mru, i); break end
end
pads_cache_mru[#pads_cache_mru + 1] = id
local guard = 0
while #pads_cache_mru > PADS_CACHE_MAX and guard < PADS_CACHE_MAX + 4 do
guard = guard + 1
local oldest = table.remove(pads_cache_mru, 1)
local has_unsaved_write = pending_pad_saves[oldest] ~= nil
if oldest == id or has_unsaved_write then

pads_cache_mru[#pads_cache_mru + 1] = oldest
else
pads_cache[oldest] = nil
end
end
end
function trim_pad_cache()
local keep = current_target_id()
for pid in pairs(pads_cache) do
if pid ~= keep and not pending_pad_saves[pid] then pads_cache[pid] = nil end
end
pads_cache_mru = {}
for pid in pairs(pads_cache) do pads_cache_mru[#pads_cache_mru + 1] = pid end
end
function pad_hex_file_key(id)
return "otp3pad_" .. sanitize_id(id)
end
function pad_meta_key(id)
return "padmeta_" .. sanitize_id(id)
end

function serialize_pad_meta(kind, rec)
local full_len = rec.bytes and #rec.bytes or 0
if kind == "channel" then
local remaining = full_len - (rec.ctr or 0)
return "C2|" .. tostring(rec.ctr or 0) .. "|" .. tostring(remaining)
end
local remaining = full_len - (rec.ctr_mine or 0) - (rec.ctr_theirs or 0)
return "D2|" .. rec.role .. "|" .. tostring(rec.ctr_mine or 0) .. "|" .. tostring(rec.ctr_theirs or 0) .. "|" .. tostring(remaining)
end

function parse_pad_meta(kind, raw)
if type(raw) ~= "string" or raw == "" then
if kind == "channel" then return { ctr = 0, bytes = nil } end
return { role = "A", ctr_mine = 0, ctr_theirs = 0, bytes = nil }
end
if kind == "channel" then
local ctr, bytes = raw:match("^C2|(%d+)|(%d+)$")
if ctr then return { ctr = tonumber(ctr) or 0, bytes = tonumber(bytes) } end
local ctr_old = raw:match("^C|(%d+)|%d+$") or raw:match("^C|(%d+)$")
if ctr_old then return { ctr = tonumber(ctr_old) or 0, bytes = nil, legacy_format = true } end
return { ctr = 0, bytes = nil }
end
local role, cm, ct, bytes = raw:match("^D2|([AB])|(%d+)|(%d+)|(%d+)$")
if role then return { role = role, ctr_mine = tonumber(cm) or 0, ctr_theirs = tonumber(ct) or 0, bytes = tonumber(bytes) } end
local role_old, cm_old, ct_old = raw:match("^D|([AB])|(%d+)|(%d+)|%d+$")
if not role_old then role_old, cm_old, ct_old = raw:match("^D|([AB])|(%d+)|(%d+)$") end
if not role_old then return { role = "A", ctr_mine = 0, ctr_theirs = 0, bytes = nil } end
return { role = role_old, ctr_mine = tonumber(cm_old) or 0, ctr_theirs = tonumber(ct_old) or 0, bytes = nil, legacy_format = true }
end
function save_pad_meta(id, kind, rec)
sset_plain(pad_meta_key(id), serialize_pad_meta(kind, rec))
pad_hint_cache_invalidate(id)
end

function try_write_pad_hex(id, hex)
local key = pad_hex_file_key(id)
if wada.fs then
local ok, wrote = pcall(function() return wada.fs.write(key .. ".hex", enc_pad(hex)) end)
if ok and wrote then return true end
return false
end
if #hex > STORE_SAFE_HEX_CHARS then return false end
return (pcall(sset, key, hex))
end

function try_persist_pad(id, rec)
if rec._hex_dirty then
local ok_hex, hex = pcall(b2h, rec.bytes)
if not ok_hex then return false end
if not try_write_pad_hex(id, hex) then return false end
rec._hex_dirty = false
end
local meta_ok = pcall(sset_plain, pad_meta_key(id), serialize_pad_meta(rec.kind, rec))
if meta_ok then pad_hint_cache_invalidate(id) end
return meta_ok == true
end
function save_pad(id, rec)
if try_persist_pad(id, rec) then

pending_pad_saves[id] = nil
return
end

pending_pad_saves[id] = pending_pad_saves[id] or { id = id, retry_count = 0 }
pending_pad_saves[id].last_attempt_ms = wada.sys.millis()
wada.sys.toast("save rate-limited, retrying automatically...")
end
function pad_save_tick()
for id, p in pairs(pending_pad_saves) do
if wada.sys.millis() - p.last_attempt_ms >= PAD_SAVE_RETRY_BACKOFF_MS then
p.retry_count = p.retry_count + 1
p.last_attempt_ms = wada.sys.millis()
local rec = pads_cache[id]
if not rec then

pending_pad_saves[id] = nil
elseif try_persist_pad(id, rec) then
pending_pad_saves[id] = nil
elseif p.retry_count >= PAD_SAVE_MAX_RETRIES then
pending_pad_saves[id] = nil
wada.sys.toast("could not save pad state after retrying - please resend")
else
wada.sys.toast(string.format("still rate-limited, retrying (%d/%d)...", p.retry_count, PAD_SAVE_MAX_RETRIES))
end
end
end
end

function load_pad_hex(id)
local key = pad_hex_file_key(id)
if wada.fs then
local ok, data = pcall(wada.fs.read, key .. ".hex")
if ok and type(data) == "string" and data ~= "" then
return dec_pad(data), true
end
if not ok then
return "", false
end
return "", true
end
return sget(key, ""), true
end
local legacy_pad_warned = {}
local read_failed_warned = {}

local function try_load_pad_bytes(id)
for attempt = 1, 3 do
local hex, read_ok = load_pad_hex(id)
if read_ok then
local ok_bytes, bytes = pcall(h2b, hex)
if ok_bytes and bytes then return bytes, true end
end
end
return "", false
end
function load_pad(id, kind)
known_pad_ids[id] = true
local cached = pads_cache[id]
if cached then
pads_cache_touch(id)
return cached
end
local bytes, read_ok = try_load_pad_bytes(id)
local meta = parse_pad_meta(kind, sget_plain(pad_meta_key(id), ""))
local rec = { bytes = bytes, kind = kind }
if kind == "channel" then
rec.ctr = meta.ctr
else
rec.role = meta.role
rec.ctr_mine = meta.ctr_mine
rec.ctr_theirs = meta.ctr_theirs
end

if meta.legacy_format and bytes ~= "" then
rec._legacy_format = true
if not legacy_pad_warned[id] then
legacy_pad_warned[id] = true
wada.sys.toast("pad for " .. id .. " is from an older build - wipe and reimport before sending/receiving")
end
end

local consumed_total = (kind == "channel") and (rec.ctr or 0) or ((rec.ctr_mine or 0) + (rec.ctr_theirs or 0))
if not rec._legacy_format and (not read_ok or (#bytes == 0 and consumed_total > 0)) then
rec._read_failed = true
if not read_failed_warned[id] then
read_failed_warned[id] = true
wada.sys.toast("could not read pad for " .. id .. " from storage - tap ReSync to retry, or wipe and reimport if this keeps happening")
end
end
pads_cache[id] = rec
pads_cache_touch(id)
return rec
end

function emmb(rec)
if rec._legacy_format or rec._read_failed then return 0 end
if rec.kind == "channel" then
return #rec.bytes - (rec.ctr or 0)
end
return #rec.bytes - (rec.ctr_mine or 0) - (rec.ctr_theirs or 0)
end

function pad_debug_str(rec, kind)
if not rec then return "no pad" end
if kind == "channel" then
return string.format("ctr=%d bytes=%d", rec.ctr or 0, emmb(rec))
end
return string.format("role=%s mine=%d theirs=%d bytes=%d", tostring(rec.role), rec.ctr_mine or 0, rec.ctr_theirs or 0, emmb(rec))
end
local pad_hint_cache = {}
function pad_byte_count_hint(id, kind)
local cached = pads_cache[id]
if cached then return emmb(cached) end
local h = pad_hint_cache[id]
if h ~= nil then return h end
local meta = parse_pad_meta(kind, sget_plain(pad_meta_key(id), ""))
pad_hint_cache[id] = meta.bytes
return meta.bytes
end
function pad_hint_cache_invalidate(id)
pad_hint_cache[id] = nil
end
function pdib(rec, dir)
if rec.kind == "channel" then return false end
if rec.role == "A" then return dir == "theirs" end
return dir == "mine"
end

function pks(rec, n, dir)
if rec._legacy_format or rec._read_failed then return nil end
local ok, result = pcall(function()
if rec.kind == "channel" then
local start = rec.ctr or 0
return b2h(rec.bytes:sub(start + 1, start + n))
end
local consumed = (dir == "mine") and (rec.ctr_mine or 0) or (rec.ctr_theirs or 0)
if pdib(rec, dir) then
local start = #rec.bytes - consumed - n
return b2h(rec.bytes:sub(start + 1, start + n))
else
return b2h(rec.bytes:sub(consumed + 1, consumed + n))
end
end)
if not ok then return nil end
return result
end

local last_cons_pad_error = nil
function TEST_get_last_cons_pad_error()
return last_cons_pad_error
end

function cons_pad(id, rec, n, dir)
if rec._legacy_format then
last_cons_pad_error = "pad is from an older build - wipe and reimport"
return false
end
if rec._read_failed then
last_cons_pad_error = "could not read pad from storage - tap ReSync to retry"
return false
end
local old_ctr, old_ctr_mine, old_ctr_theirs = rec.ctr, rec.ctr_mine, rec.ctr_theirs
local ok, err = pcall(function()
if rec.kind == "channel" then
rec.ctr = (rec.ctr or 0) + n
elseif dir == "mine" then
rec.ctr_mine = (rec.ctr_mine or 0) + n
else
rec.ctr_theirs = (rec.ctr_theirs or 0) + n
end
end)
if not ok then
last_cons_pad_error = "consume: " .. tostring(err)
return false
end
local save_ok, save_err = pcall(save_pad, id, rec)
if not save_ok then
rec.ctr, rec.ctr_mine, rec.ctr_theirs = old_ctr, old_ctr_mine, old_ctr_theirs
last_cons_pad_error = "save: " .. tostring(save_err)
return false
end
last_cons_pad_error = nil
return true
end
function wipe_pad(id, kind)
local rec = load_pad(id, kind)
rec.bytes = ""
rec._hex_dirty = true
if kind == "channel" then rec.ctr = 0 else rec.role = rec.role or "A"; rec.ctr_mine = 0; rec.ctr_theirs = 0 end
save_pad(id, rec)
clear_sync_state(id)
end
function wipe_all_pads()

for id in pairs(known_pad_ids) do
local kind = pad_kind_from_id(id)
if kind then
local rec = load_pad(id, kind)
rec.bytes = ""
rec._hex_dirty = true
if kind == "channel" then rec.ctr = 0 else rec.ctr_mine = 0; rec.ctr_theirs = 0 end
save_pad(id, rec)
clear_sync_state(id)
end
end
end
local keep_pad_files = true
local show_participant_strip = false
local waiting_queue = {}
local WAITING_QUEUE_CAP = 5
function import_pad_hex(id, kind, hex, role)
if kind == "channel" and is_public_channel_id(id) then
return false, "OTP is not available on the Public channel"
end
if #hex % 2 ~= 0 then
return false, "that pad has an odd number of hex characters - not a whole number of bytes, refusing to import"
end
if #hex / 2 > MXADD then
return false, string.format("%d bytes is too much for one import (max %d)", math.floor(#hex / 2), MXADD)
end
if waiting_queue[id] and #waiting_queue[id] > 0 then
return false, string.format("%d message(s) are still waiting for a path on the current pad - send or cancel them before importing a new one", #waiting_queue[id])
end
local bytes, herr = h2b(hex)
if not bytes then
return false, "that pad isn't valid hex (" .. tostring(herr) .. ") - refusing to import"
end
local rec = { bytes = bytes, kind = kind, _hex_dirty = true }
if kind == "channel" then
rec.ctr = 0
else
rec.role = role or "A"
rec.ctr_mine, rec.ctr_theirs = 0, 0
end
pads_cache[id] = rec
known_pad_ids[id] = true
pads_cache_touch(id)
save_pad(id, rec)
clear_sync_state(id)
return true
end
local importable_files_cache = nil
function invalidate_importable_files_cache()
importable_files_cache = nil
end
function known_pad_storage_filenames()
local known = {}
for _, c in ipairs(p_cts()) do
known[pad_hex_file_key(target_id("dm", contact_key(c))) .. ".hex"] = true
end
local ok, names = pcall(wada.mesh.channels)
if ok and type(names) == "table" then
for _, nm in ipairs(names) do
known[pad_hex_file_key(target_id("channel", nm)) .. ".hex"] = true
end
end
return known
end
function scan_importable_files()
if importable_files_cache then return importable_files_cache end
if not wada.fs or type(wada.fs.list) ~= "function" then return {} end
local ok, files = pcall(wada.fs.list)
if not ok or type(files) ~= "table" then return {} end
local known = known_pad_storage_filenames()
local out = {}
for _, f in ipairs(files) do
if type(f) == "table" and type(f.name) == "string" and not known[f.name] then
local lower = f.name:lower()
local kind
if lower:sub(-4) == ".hex" then
kind = "hex"
elseif lower:sub(-4) == ".txt" then
kind = "text"
end
if kind then
table.insert(out, { filename = f.name, kind = kind })
end
end
end
importable_files_cache = out
return out
end
function import_pad_file(entry, id, kind, role)
local ok, raw = pcall(wada.fs.read, entry.filename)
if not ok or type(raw) ~= "string" then
return false, "could not read " .. entry.filename
end
local decoded = dec_pad(raw)
local hex
if entry.kind == "text" then
if #decoded > MXADD then
return false, string.format("%s is %d bytes - too much for one import (max %d)", entry.filename, #decoded, MXADD)
end
local ok_hex, result = pcall(b2h, decoded)
if not ok_hex then return false, "could not import right now (" .. clean_err(result) .. ")" end
hex = result
else
if not decoded:match("^[0-9a-fA-F]*$") then
return false, entry.filename .. " does not look like valid hex - skipped"
end
if #decoded % 2 ~= 0 then
return false, entry.filename .. " has an odd number of hex characters - not a whole number of bytes, skipped"
end
if #decoded / 2 > MXADD then
return false, string.format("%s is %d bytes - too much for one import (max %d)", entry.filename, math.floor(#decoded / 2), MXADD)
end
hex = decoded
end
local ok2, err2 = import_pad_hex(id, kind, hex, role)
if not ok2 then return false, err2 end
if wada.fs and not keep_pad_files then pcall(wada.fs.remove, entry.filename) end
invalidate_importable_files_cache()
return true
end
local PAD_GEN_SLICE = 500
local pad_gen_remaining = 0
local pad_gen_target_id, pad_gen_target_kind = nil, nil
local pad_gen_buffer = {}
function start_pad_generation(id, kind, target_bytes)
if kind == "channel" and is_public_channel_id(id) then
wada.sys.toast("OTP is not available on the Public channel")
return
end
if pad_gen_remaining > 0 then
wada.sys.toast("already generating, please wait")
return
end
local rec = load_pad(id, kind)
local current_bytes = emmb(rec)
if current_bytes >= MAX_TOTAL_PAD_BYTES then
wada.sys.toast(string.format("pad already has %d bytes (cap is %d) - wipe or use some pad before generating more", current_bytes, MAX_TOTAL_PAD_BYTES))
return
end
if current_bytes + target_bytes > MAX_TOTAL_PAD_BYTES then
target_bytes = MAX_TOTAL_PAD_BYTES - current_bytes
end
pad_gen_target_id, pad_gen_target_kind = id, kind
pad_gen_remaining = target_bytes
pad_gen_buffer = {}
wada.sys.toast("generating " .. target_bytes .. " bytes...")
end
function pad_gen_tick()
if pad_gen_remaining <= 0 then return end
local this_slice = math.min(PAD_GEN_SLICE, pad_gen_remaining)
local ok, err = pcall(function()
local out = {}
for i = 1, this_slice do
out[i] = string.char(rand_byte())
end
table.insert(pad_gen_buffer, table.concat(out))
end)
pad_gen_remaining = pad_gen_remaining - this_slice
if not ok or pad_gen_remaining <= 0 then
local rec = load_pad(pad_gen_target_id, pad_gen_target_kind)
rec.bytes = rec.bytes .. table.concat(pad_gen_buffer)
rec._hex_dirty = true
pad_gen_buffer = {}
save_pad(pad_gen_target_id, rec)
if current_screen == "pad" then refresh_pad_status() end
if not ok then
pad_gen_remaining = 0
wada.sys.toast("pad generation stopped (" .. clean_err(err) .. ") - " .. emmb(rec) .. " bytes saved so far")
else
wada.sys.toast("pad generation complete (unverified RNG)")
if current_screen == "pad" then goto_screen("chat") end
end
end
end
function is_person_contact(c)
if type(c) ~= "table" then return false end
local t = c.type
local NREP = wada.mesh.NODE_REPEATER or 2
local NROOM = wada.mesh.NODE_ROOM or 3
local NSENS = wada.mesh.NODE_SENSOR or 4
if type(t) == "number" and (t == NREP or t == NROOM or t == NSENS) then
return false
end
if type(t) == "string" then
local tl = t:lower()
if tl:find("repeat", 1, true) or tl:find("room", 1, true) or tl:find("sensor", 1, true) then
return false
end
end
return true
end

local P_CTS_TTL_MS = 4000
local p_cts_cache, p_cts_cache_at = nil, nil
local p_cts_last_err = nil
function p_cts_uncached()
local out = {}
local count_ok, total = pcall(wada.mesh.contact_count)
local raw_all = {}
if not count_ok or type(total) ~= "number" then
local ok, contacts = pcall(wada.mesh.contacts)
if ok and contacts then
raw_all = contacts
elseif not ok then
p_cts_last_err = tostring(contacts)
end
else
local ok, contacts = pcall(wada.mesh.contacts, 0, 100)
if ok and contacts then
raw_all = contacts
elseif not ok then
p_cts_last_err = tostring(contacts)
end
end
if #raw_all > 0 then p_cts_last_err = nil end
for _, c in ipairs(raw_all) do
if is_person_contact(c) then out[#out + 1] = c end
end
return out
end
function p_cts()
local now = wada.sys.millis()
if p_cts_cache and p_cts_cache_at and #p_cts_cache > 0 and (now - p_cts_cache_at) < P_CTS_TTL_MS then
return p_cts_cache
end
local fresh = p_cts_uncached()
if #fresh > 0 or not p_cts_cache or #p_cts_cache == 0 then
p_cts_cache = fresh
p_cts_cache_at = now
end
return p_cts_cache
end
function contact_key(c)
if type(c.pubkey) == "string" and c.pubkey ~= "" then return c.pubkey end
return c.name
end
function get_self_pubkey()
local ok, me = pcall(wada.mesh.self)
if not ok or type(me) ~= "table" then return nil, nil end
for _, field in ipairs({ "pubkey", "public_key", "id" }) do
local v = me[field]
if type(v) == "string" and v ~= "" then return v, field end
end
return nil, nil
end
function resolve_dm_role(target)
if type(target) ~= "table" then return nil end
local mine = get_self_pubkey()
local theirs = target.pubkey
if type(mine) ~= "string" or mine == "" then return nil end
if type(theirs) ~= "string" or theirs == "" then return nil end
mine = mine:lower()
theirs = theirs:lower()
if mine <= theirs then return "A" end
return "B"
end
local dkind, dtarget = "channel", ""

local dm_id_cache = nil
function pin_dm_target_id()
if type(dtarget) ~= "table" then dm_id_cache = nil; return end
local live = find_contact_by_name(dtarget.name) or dtarget
dm_id_cache = target_id("dm", contact_key(live))
end

function otp_active_for_current_chat()
return not (dkind == "channel" and is_public_channel_name(dtarget))
end
function current_target_id()
if dkind == "dm" then
if type(dtarget) ~= "table" then return nil end

if dm_id_cache == nil then pin_dm_target_id() end
return dm_id_cache
end
if dtarget == "" then return nil end
return target_id("channel", dtarget)
end
function current_target_label()
if dkind == "dm" then
return (type(dtarget) == "table" and dtarget.name) or "(no contact set)"
end
return (dtarget ~= "" and ("#" .. normalize_channel(dtarget))) or "(no channel set)"
end
function current_pad()
local id = current_target_id()
if not id then return nil, nil end

if dkind == "dm" and sget_plain(pad_meta_key(id), "") == "" then
local auto_role = resolve_dm_role(dtarget)
if auto_role then

local ok = pcall(save_pad_meta, id, "dm", { role = auto_role, ctr_mine = 0, ctr_theirs = 0 })
if ok then
pads_cache[id] = nil
end
end
end
return load_pad(id, dkind), id
end
local sync_status_by_target = {}
local channel_participants = {}
local PARTICIPANT_MAX_AGE_MS = 10 * 60 * 1000
function prune_stale_participants(per_channel, now)
for name, entry in pairs(per_channel) do
if now - entry.last_heard_ms > PARTICIPANT_MAX_AGE_MS then
per_channel[name] = nil
end
end
end

function record_channel_participant_heard(id, sender, claimed_offset, current_ctr)
if not sender then return end
local per_channel = channel_participants[id]
if not per_channel then
per_channel = {}
channel_participants[id] = per_channel
end
local now = wada.sys.millis()
prune_stale_participants(per_channel, now)
local entry = per_channel[sender] or {}
entry.last_heard_ms = now
if claimed_offset ~= nil then
entry.in_sync = (claimed_offset == current_ctr)
end
per_channel[sender] = entry
end
function channel_participants_prune_empty()
for id, per_channel in pairs(channel_participants) do
prune_stale_participants(per_channel, wada.sys.millis())
if next(per_channel) == nil then
channel_participants[id] = nil
end
end
end

local CHANNEL_PARTICIPANTS_PRUNE_INTERVAL_MS = 5 * 60 * 1000
local next_channel_participants_prune_ms = 0
function channel_participants_prune_tick()
local now = wada.sys.millis()
if now < next_channel_participants_prune_ms then return end
next_channel_participants_prune_ms = now + CHANNEL_PARTICIPANTS_PRUNE_INTERVAL_MS
channel_participants_prune_empty()
end
function active_channel_participants(id)
local per_channel = id and channel_participants[id]
if not per_channel then return {} end
local now = wada.sys.millis()
local out = {}
for name, entry in pairs(per_channel) do
if now - entry.last_heard_ms <= PARTICIPANT_MAX_AGE_MS then
out[#out + 1] = { name = name, in_sync = entry.in_sync }
end
end
table.sort(out, function(a, b) return a.name:lower() < b.name:lower() end)
return out
end
function send_sync_check(id, kind, rec, is_reply)
local payload
if kind == "channel" then
payload = SYNC_MARKER .. (is_reply and "S" or "s") .. to_base36(rec.ctr or 0)
else
payload = SYNC_MARKER .. (is_reply and "P" or "p") .. (rec.role or "A") .. to_base36(rec.ctr_mine or 0) .. ":" .. to_base36(rec.ctr_theirs or 0)
end
local override_target = (kind == "dm") and (type(dtarget) == "table" and dtarget.name or nil) or dtarget
raw_transmit(payload, kind, override_target)
end
local RESYNC_GRACE_MS = 12000
local pending_resync = {}  

local resync_streak = {}  
local resync_badge = {}  
local RESYNC_BADGE_MS = 6000
function show_resync_badge(id, skipped)
resync_badge[id] = { text = string.format("resynced - skipped %d B", skipped), until_ms = wada.sys.millis() + RESYNC_BADGE_MS }
if current_screen == "chat" and current_target_id() == id then refresh_chat_bytes() end
end
function resync_badge_tick()
local id = current_target_id()
if not id then return end
local badge = resync_badge[id]
if not badge then return end
if wada.sys.millis() >= badge.until_ms then
resync_badge[id] = nil
if current_screen == "chat" then refresh_chat_bytes() end
end
end
local sync_backoff = {}  
local awaiting_reply_since_ms = {}  
local psend = nil
local imt_by_target = {}
local ssend = nil
local pending_channel_send = nil
local CHANNEL_JITTER_MIN_MS = 600
local CHANNEL_JITTER_MAX_MS = 2200
function channel_jitter_delay_ms()
return CHANNEL_JITTER_MIN_MS + math.random(0, CHANNEL_JITTER_MAX_MS - CHANNEL_JITTER_MIN_MS)
end
local SYNC_PROMPT_MIN_SPACING_MS = 10000
local MAX_AUTO_RESYNC_ABS_BYTES = 500
local last_sync_check_sent_ms = {}
function handle_sync_message(text, sender, kind_hint, msg_id)
local rest = text:sub(#SYNC_MARKER + 1)
local mode_char = rest:sub(1, 1)
local body = rest:sub(2)
local msg_is_channel = (mode_char == "s" or mode_char == "S")
local msg_is_dm = (mode_char == "p" or mode_char == "P")
if not (msg_is_channel or msg_is_dm) then return end
local is_reply = (mode_char == "S" or mode_char == "P")

if msg_id and msg_id ~= current_target_id() then return end
if msg_is_channel ~= (dkind == "channel") then
sync_status_by_target[current_target_id() or ""] = { ok = false, text = "party mode mismatch" }
if current_screen == "pad" then refresh_pad_status(); refresh_pad_sync_label(); refresh_pad_detail_label() end
return
end
local rec, id = current_pad()
if not rec or not id then return end
sync_backoff[id] = nil
awaiting_reply_since_ms[id] = nil
if msg_is_dm then flush_waiting(id) end
local ok, text_out
local my_recv_ctr, their_send_ctr, resync_kind
local role_collision = false
if msg_is_channel then
local their_offset = from_base36(body)
if not their_offset then return end
my_recv_ctr, their_send_ctr, resync_kind = rec.ctr or 0, their_offset, "channel"
ok = (their_offset == my_recv_ctr)
text_out = ok and "in sync" or string.format("out of sync (you: %d, %s: %d)", my_recv_ctr, sender or "them", their_offset)
else
local their_role, mine_str, theirs_str = body:match("^([AB])([0-9a-zA-Z]+):([0-9a-zA-Z]+)$")
if not their_role then return end
local their_mine, their_theirs = from_base36(mine_str), from_base36(theirs_str)
if not their_mine or not their_theirs then return end
my_recv_ctr, their_send_ctr, resync_kind = rec.ctr_theirs or 0, their_mine, "dm"

if their_role == rec.role then
role_collision = true
ok = false
text_out = string.format("ROLE COLLISION: both sides resolved role %s - wipe and re-import this pad on one device", rec.role or "?")
else
ok = (their_mine == my_recv_ctr) and (their_theirs == (rec.ctr_mine or 0))
text_out = ok and "in sync" or string.format("out of sync (you: %d/%d, them: %d/%d)", rec.ctr_mine or 0, my_recv_ctr, their_mine or -1, their_theirs or -1)
end
end
sync_status_by_target[id] = { ok = ok, text = text_out }
if current_screen == "pad" then refresh_pad_status(); refresh_pad_sync_label(); refresh_pad_detail_label() end

if ok then resync_streak[id] = nil end
if not role_collision and msg_is_dm and (their_theirs or 0) > (rec.ctr_mine or 0) then
local d = their_theirs - (rec.ctr_mine or 0)
if d <= math.min(emmb(rec), MAX_AUTO_RESYNC_ABS_BYTES) and cons_pad(id, rec, d, "mine") then
ok = false
text_out = string.format("auto-resynced send side (skipped %d bytes)", d)
sync_status_by_target[id] = { ok = true, text = text_out }
show_resync_badge(id, d)
if current_screen == "pad" then refresh_pad_status(); refresh_pad_sync_label(); refresh_pad_detail_label() end
if is_reply then send_sync_check(id, dkind, rec, false) end
end
end
if role_collision then

pending_resync[id] = nil
else
local gap = their_send_ctr - my_recv_ctr
if gap > 0 then
local pending = pending_resync[id]
if not (pending and pending.peer_ctr == their_send_ctr and pending.kind == resync_kind) then
pending_resync[id] = { peer_ctr = their_send_ctr, kind = resync_kind, first_seen_ms = wada.sys.millis() }
end
else
pending_resync[id] = nil
end
end
if not is_reply then
local now = wada.sys.millis()
local last_sent = last_sync_check_sent_ms[id]
if not (last_sent and now - last_sent < SYNC_PROMPT_MIN_SPACING_MS) then
send_sync_check(id, dkind, rec, true)
last_sync_check_sent_ms[id] = now
end
end
end

local MAX_AUTO_RESYNC_FRACTION = 0.5
local MAX_CONSECUTIVE_AUTO_RESYNCS = 3
function auto_sync_tick()
local id = current_target_id()
if not id then return end
local pending = pending_resync[id]
if not pending then return end
if wada.sys.millis() - pending.first_seen_ms < RESYNC_GRACE_MS then return end
local rec = load_pad(id, pending.kind)
local my_recv_ctr_now = (pending.kind == "channel") and (rec.ctr or 0) or (rec.ctr_theirs or 0)
local gap = pending.peer_ctr - my_recv_ctr_now
if gap <= 0 then
pending_resync[id] = nil
resync_streak[id] = nil
return
end
if pending.kind == "dm" and (resync_streak[id] or 0) >= MAX_CONSECUTIVE_AUTO_RESYNCS then
pending_resync[id] = nil
sync_status_by_target[id] = { ok = false, text = string.format("repeated resync with no real fix (role %s) - check both sides show complementary A/B roles, or wipe and re-import", rec.role or "?") }
if current_screen == "pad" then refresh_pad_status(); refresh_pad_sync_label(); refresh_pad_detail_label() end
return
end
local avail = emmb(rec)
local cap = math.min(math.floor(avail * MAX_AUTO_RESYNC_FRACTION), MAX_AUTO_RESYNC_ABS_BYTES)
if avail < gap then
pending_resync[id] = nil
sync_status_by_target[id] = { ok = false, text = string.format("out of sync by %d bytes - not enough pad left to auto-fix, wipe and re-import", gap) }
if current_screen == "pad" then refresh_pad_status(); refresh_pad_sync_label(); refresh_pad_detail_label() end
return
end
if gap > cap then
pending_resync[id] = nil
sync_status_by_target[id] = { ok = false, text = string.format("out of sync by %d bytes - too large to auto-fix safely (would burn most of what's left), wipe and re-import", gap) }
if current_screen == "pad" then refresh_pad_status(); refresh_pad_sync_label(); refresh_pad_detail_label() end
return
end
local skipped = gap
if not cons_pad(id, rec, skipped, "theirs") then
pending_resync[id] = nil

sync_status_by_target[id] = { ok = false, text = "auto-resync failed: " .. tostring(TEST_get_last_cons_pad_error()) }
if current_screen == "pad" then refresh_pad_status(); refresh_pad_sync_label(); refresh_pad_detail_label() end
wada.sys.toast("auto-resync failed: " .. tostring(TEST_get_last_cons_pad_error()))
return
end
pending_resync[id] = nil
resync_streak[id] = (resync_streak[id] or 0) + 1
sync_status_by_target[id] = { ok = true, text = string.format("auto-resynced (skipped %d bytes - a message may have been lost)", skipped) }
if current_screen == "pad" then refresh_pad_status(); refresh_pad_sync_label(); refresh_pad_detail_label() end
show_resync_badge(id, skipped)
send_sync_check(id, dkind, rec, false)
end
local AUTO_SYNC_INTERVAL_MS = 300000
local next_sync_check_ms = {}
local SYNC_BACKOFF_MS = 30000
function request_prompt_sync_check(id)
if not id then return end
sync_backoff[id] = nil
awaiting_reply_since_ms[id] = nil
next_sync_check_ms[id] = 0
end
function resync_current_target()
local id = current_target_id()
if not id then
wada.sys.toast("no contact/channel selected")
return
end

if not pending_pad_saves[id] then
pads_cache[id] = nil
end
local rec = load_pad(id, dkind)
request_prompt_sync_check(id)
send_sync_check(id, dkind, rec, false)
refresh_chat_bytes()

update_compose_preview_and_counter()
wada.sys.toast("pad reloaded - sync check sent (" .. pad_debug_str(rec, dkind) .. ")")
end
function maybe_auto_sync_check()
if current_screen ~= "chat" then return end
local id = current_target_id()
if not id then return end
if sync_backoff[id] then return end
local now = wada.sys.millis()
if now < (next_sync_check_ms[id] or 0) then return end
local last_sent = last_sync_check_sent_ms[id]
if last_sent and now - last_sent < SYNC_PROMPT_MIN_SPACING_MS then return end
if psend and psend.pad_id == id then return end
local imt = imt_by_target[id]
if imt and not imt.completed then return end
if not airtime_clear() then return end
local rec = load_pad(id, dkind)
send_sync_check(id, dkind, rec, false)
next_sync_check_ms[id] = now + AUTO_SYNC_INTERVAL_MS
awaiting_reply_since_ms[id] = now
last_sync_check_sent_ms[id] = now
end
function sync_backoff_tick()
local id = current_target_id()
if not id then return end
local since = awaiting_reply_since_ms[id]
if not since then return end
if wada.sys.millis() - since >= SYNC_BACKOFF_MS then
sync_backoff[id] = true
awaiting_reply_since_ms[id] = nil
end
end
function clear_sync_state(id)
pending_resync[id] = nil
sync_status_by_target[id] = nil
next_sync_check_ms[id] = nil
sync_backoff[id] = nil
awaiting_reply_since_ms[id] = nil
last_sync_check_sent_ms[id] = nil
imt_by_target[id] = nil
resync_streak[id] = nil
end
local MCPB, MC_MAXNAME = 160, 32
function est_max_wire_chars()
local name_len = MC_MAXNAME
local ok, sinfo = pcall(wada.mesh.self)
if ok and sinfo and sinfo.name then
name_len = #tostring(sinfo.name)
end
local overhead = name_len + 2 + 4 + 1
return math.max(20, MCPB - overhead)
end
function max_singleshot_bytes()
return est_max_wire_chars() // 2
end
function max_chunk_bytes()
return math.max(1, max_singleshot_bytes() - #MARKER.msg_chunk - 3)
end
function multi_wire_cap()
return math.min(65, max_singleshot_bytes())
end
local DUTY_CEILING = 99
function airtime_clear()
local ok, st = pcall(wada.mesh.stats)
if not ok or not st then return true end
if st.tx_budget_ms ~= nil then return st.tx_budget_ms > 0 end
if st.duty_pct ~= nil then return st.duty_pct < DUTY_CEILING end
return true
end
local FAIL_WORDS = { C = "collided", D = "decrypt error", G = "garbled" }
local nack_queue = {}
local last_nack_ms = 0
REASM_GARBAGE = false
function queue_nack(kind, target, reason)
if not target then return end
local payload = "z4" .. reason .. "z"
for _, q in ipairs(nack_queue) do
if q.payload == payload then return end
end
nack_queue[#nack_queue + 1] = { payload = payload, kind = kind, target = target }
while #nack_queue > 2 do table.remove(nack_queue, 1) end
end
function nack_tick()
if #nack_queue == 0 then return end
local now = wada.sys.millis()
if now - last_nack_ms < 5500 or not airtime_clear() then return end
local q = nack_queue[1]
last_nack_ms = now
local ok = raw_transmit(q.payload, q.kind, q.target)
q.tries = (q.tries or 0) + 1
if ok or q.tries >= 4 then table.remove(nack_queue, 1) end
end
local last_rep_ms = 0
function repeat_tick()
local now = wada.sys.millis()
if now - last_rep_ms < 2000 or current_screen ~= "chat" then return end
last_rep_ms = now
local id = current_target_id()
if not id then return end
local log = chat_log_for(id)
local seen = 0
for i = #log, 1, -1 do
local e = log[i]
if e.who == "You" and e.fp then
seen = seen + 1
if seen > 3 then break end
e.rep_t0 = e.rep_t0 or now
if now - e.rep_t0 < 90000 then
local ok, n = pcall(wada.mesh.repeats, e.fp)
if ok and type(n) == "number" and n > (e.rep or 0) then
e.rep = n
refresh_chat_view()
end
end
end
end
end
function handle_nack(text, who, id)
local reason = text:match("z4(%u)z$")
if not reason or not id then return end
local log = chat_log_for(id)
for i = #log, 1, -1 do
local e = log[i]
if e.who == "You" and (e.kind == "sent" or e.kind == "pending" or e.kind == "retrying") then
if reason == "C" and not e.retried and not pending_channel_send and dkind == "channel" and current_target_id() == id then
e.retried, e.kind, e.fp, e.rep, e.rep_t0 = true, "retrying", nil, nil, nil
pending_channel_send = { id = id, text = e.text, target_name = dtarget, log_entry = e, fire_at_ms = wada.sys.millis() + 2000 + math.random(0, 4000) }
if current_screen == "chat" then refresh_chat_view() end
return
end
e.kind = "failed"
e.fail = reason
e.fail_who = who
if psend and psend.log_entry == e then psend = nil end
if ssend and ssend.log_entry == e then ssend = nil end
if current_screen == "chat" and current_target_id() == id then refresh_chat_view() end
return
end
end
end
function raw_transmit(cipher_hex, use_kind, override_target, channel_offset)
local pok, sent_ok, err
local fingerprint = nil
local wire_text = cipher_hex
if use_kind == "channel" and channel_offset ~= nil then
wire_text = CHANNEL_OFFSET_MARKER .. to_base36(channel_offset) .. "|" .. cipher_hex
end
if use_kind == "dm" then
local target = override_target
if type(wada.mesh.send_dm) ~= "function" then return false, "send_dm unavailable on this firmware", nil end
pok, sent_ok, err = pcall(wada.mesh.send_dm, target, wire_text)
else
local target = override_target
if type(wada.mesh.send) ~= "function" then return false, "send unavailable on this firmware", nil end
pok, sent_ok, err = pcall(wada.mesh.send, target, wire_text)
if pok and sent_ok == true then
fingerprint = err
err = nil
end
end
if not pok then return false, "send crashed: " .. tostring(sent_ok), nil end
if not sent_ok then return false, "send failed: " .. tostring(err), err end
return true, nil, nil, fingerprint
end
function llg(s)
if s == "" then return false end
local control_count, space_count, seen, dcount = 0, 0, {}, 0
local max_run, cur_run, last_byte = 0, 0, nil
for i = 1, #s do
local b = string.byte(s, i)
if b < 0x20 and b ~= 9 and b ~= 10 and b ~= 13 then
control_count = control_count + 1
end
if b == 0x20 then
space_count = space_count + 1
end
if not seen[b] then
seen[b] = true
dcount = dcount + 1
end
if b == last_byte then
cur_run = cur_run + 1
else
cur_run = 1
last_byte = b
end
if cur_run > max_run then max_run = cur_run end
end
if (control_count / #s) > 0.3 then return true end
if max_run >= 5 then return false end
if #s >= 20 and dcount >= 8 and (space_count / #s) < 0.045 then
return true
end
return false
end
local ack_cache = {}
local ACK_CACHE_MAX = 24
local ack_cache_order = {}
function send_ack_for_chunk(id, rec, tid, c_idx, ack_target)
local key = id .. ":" .. tid .. ":" .. c_idx
local cached = ack_cache[key]
if cached then
return raw_transmit(cached, "dm", ack_target)
end
local framed = MARKER.msg_ack .. string.char(tid, c_idx)
local need = #framed
local avail = emmb(rec)
if avail < need then
return false, string.format("not enough pad for ack: need %d, have %d", need, avail)
end
local key_hex = pks(rec, need, "mine")
local ok, cipher_hex, encerr = pcall(otp_encrypt, framed, key_hex)
if not ok or not cipher_hex then return false, "encrypt error: " .. tostring(ok and encerr or cipher_hex) end
if not cons_pad(id, rec, need, "mine") then return false, "could not save pad state (" .. tostring(TEST_get_last_cons_pad_error()) .. ")" end
ack_cache[key] = cipher_hex
table.insert(ack_cache_order, key)
if #ack_cache_order > ACK_CACHE_MAX then
local oldest = table.remove(ack_cache_order, 1)
ack_cache[oldest] = nil
end
return raw_transmit(cipher_hex, "dm", ack_target)
end
local queued_cipher, queued_entry, queued_kind, queued_target, queued_id, queued_offset = nil, nil, nil, nil, nil, nil

local ENCRYPT_RETRY_BACKOFF_MS = 1800
local ENCRYPT_RETRY_MAX = 6
local pending_encrypt_retry = nil 

function finish_single_send(id, kind, target_name, cipher_hex, log_entry)
if log_entry then log_entry.kind = "sent" end
refresh_chat_view()
if airtime_clear() then
local sent_ok, sent_err, sent_err_code, fingerprint = raw_transmit(cipher_hex, kind, target_name, nil)
if sent_ok then
if log_entry and fingerprint then log_entry.fp = fingerprint end
wada.sys.toast("sent")
request_prompt_sync_check(id)
else
if log_entry then
log_entry.kind = "retrying"
refresh_chat_view()
end
ssend = {
cipher_hex = cipher_hex, kind = kind, target = target_name, pad_id = id,
log_entry = log_entry, retry_count = 0, claim_offset = nil,
last_sent_ms = wada.sys.millis(), last_error_code = sent_err_code,
}
wada.sys.toast("send failed, will retry: " .. tostring(sent_err))
end
else
queued_entry, queued_cipher = log_entry, cipher_hex
queued_kind, queued_target, queued_id, queued_offset = kind, target_name, id, nil
refresh_chat_view()
wada.sys.toast("air busy, holding message")
end
end
function encrypt_retry_tick()
if not pending_encrypt_retry then return end
local p = pending_encrypt_retry
if wada.sys.millis() - p.last_attempt_ms < ENCRYPT_RETRY_BACKOFF_MS then return end
p.retry_count = p.retry_count + 1
p.last_attempt_ms = wada.sys.millis()
local rec = load_pad(p.id, p.kind)
local need = #p.text
local avail = emmb(rec)
if avail < need then
pending_encrypt_retry = nil
if p.log_entry then
p.log_entry.kind = "warn"
p.log_entry.text = p.log_entry.text .. " [send failed - not enough pad]"
refresh_chat_view()
end
wada.sys.toast("not enough pad left - message not sent")
return
end
local key_hex = pks(rec, need, "mine")
local ok, cipher_hex, encerr = pcall(otp_encrypt, p.text, key_hex)
if not ok or not cipher_hex then
if p.retry_count >= ENCRYPT_RETRY_MAX then
pending_encrypt_retry = nil
if p.log_entry then
p.log_entry.kind = "warn"
p.log_entry.text = p.log_entry.text .. " [send failed - encrypt error, please resend]"
refresh_chat_view()
end
wada.sys.toast("could not encrypt after retrying - please resend")
else
wada.sys.toast(string.format("still low on memory, retrying encrypt (%d/%d)...", p.retry_count, ENCRYPT_RETRY_MAX))
end
return
end
if not cons_pad(p.id, rec, need, "mine") then
pending_encrypt_retry = nil
if p.log_entry then
p.log_entry.kind = "warn"
p.log_entry.text = p.log_entry.text .. " [send failed - could not save pad state (" .. tostring(TEST_get_last_cons_pad_error()) .. ")]"
refresh_chat_view()
end
wada.sys.toast("could not save pad state - message not sent")
return
end
local id, kind, target_name, log_entry = p.id, p.kind, p.target_name, p.log_entry
pending_encrypt_retry = nil
finish_single_send(id, kind, target_name, cipher_hex, log_entry)
end
function send_chat_message(text)
if text == "" then return false end
local rec, id = current_pad()
if not id then
wada.sys.toast(dkind == "channel" and "set a channel first" or "set a DM contact first")
return false
end
if dkind == "channel" and is_public_channel_id(id) then
wada.sys.toast("OTP is not available on the Public channel")
return false
end
if psend then
wada.sys.toast("still sending the previous message, please wait")
return false
end
if ssend then
wada.sys.toast("still sending the previous message, please wait")
return false
end
if queued_cipher then
wada.sys.toast("still holding a message for clear airtime, please wait")
return false
end
if pending_channel_send then
wada.sys.toast("still sending the previous message, please wait")
return false
end
if pending_encrypt_retry then
wada.sys.toast("still encrypting the previous message, please wait")
return false
end
if dkind == "dm" and waiting_queue[id] and #waiting_queue[id] >= WAITING_QUEUE_CAP then
wada.sys.toast(string.format("%d messages to %s are already waiting for a path - send or cancel one first", #waiting_queue[id], dtarget.name))
return false
end
local is_channel = dkind == "channel"
local mwc = multi_wire_cap()
if is_channel and #text > mwc then
wada.sys.toast(string.format("too long for a channel message (max %d bytes) - use a DM to send longer messages", mwc))
return false
end
if emmb(rec) > MAX_TOTAL_PAD_BYTES then
wada.sys.toast(string.format("pad is too large to send from safely (%d bytes, cap is %d) - wipe the pad to continue", emmb(rec), MAX_TOTAL_PAD_BYTES))
return false
end
local target_name = is_channel and dtarget or dtarget.name
if is_channel and #text <= max_singleshot_bytes() then
local need = #text
local avail = emmb(rec)
if avail < need then
wada.sys.toast(string.format("not enough pad: need %d, have %d", need, avail))
return false
end
if is_channel then
local entry = a_chat("You", text, "pending")
refresh_chat_view()
pending_channel_send = {
id = id, text = text, target_name = target_name,
log_entry = entry,
fire_at_ms = wada.sys.millis() + channel_jitter_delay_ms(),
}
return true
end
end
if is_channel then
wada.sys.toast(string.format("too long for a channel message (max %d bytes) - use a DM to send longer messages", mwc))
return false
end
local ccap = max_chunk_bytes()
local chunks = {}
for i = 1, #text, ccap do
table.insert(chunks, text:sub(i, i + ccap - 1))
end
if #chunks > 255 then
wada.sys.toast("message too long even chunked (max " .. (255 * ccap) .. " chars)")
return false
end
psend = { chunks = chunks, tid = rand_byte(), tot_ch = #chunks, sctr = 0, fpay = text, kind = dkind, target = target_name, pad_id = id }
psend.log_entry = a_chat("You", text, "pending")
refresh_chat_view()
wada.sys.toast(string.format("sending %d parts...", #chunks))
send_next_chunk()
return true
end
local CHUNK_RETRY_TIMEOUT_MS = 5500
local RATE_LIMIT_BACKOFF_MS = CHUNK_RETRY_TIMEOUT_MS * 3
function send_next_chunk()
if not psend then return false end
local idx = psend.sctr
local my_tid = psend.tid
local rec = load_pad(psend.pad_id, psend.kind)
if not psend.cur_cipher or psend.cur_cipher_idx ~= idx then
local chunk_data = psend.chunks[idx + 1]
local framed = MARKER.msg_chunk .. string.char(psend.tid, idx, psend.tot_ch) .. chunk_data
local need = #framed
local avail = emmb(rec)
if avail < need then
wada.sys.toast("not enough pad mid-send")
psend = nil
return false
end
local key_hex = pks(rec, need, "mine")
local claim_offset = (psend.kind == "channel") and (rec.ctr or 0) or nil
local ok, cipher_hex = pcall(otp_encrypt, framed, key_hex)
if not ok or not cipher_hex then
wada.sys.toast("encrypt error mid-send")
psend = nil
return false
end
if not cons_pad(psend.pad_id, rec, need, "mine") then
wada.sys.toast("pad save failed mid-send (" .. tostring(TEST_get_last_cons_pad_error()) .. ")")
psend = nil
return false
end
psend.cur_cipher = cipher_hex
psend.cur_cipher_idx = idx
psend.cur_cipher_offset = claim_offset
psend.retry_count = 0
end
if not airtime_clear() then
if psend and psend.tid == my_tid and psend.sctr == idx then
psend.last_sent_ms = wada.sys.millis()
psend.last_error_code = "air busy"
end
return false
end
local cipher_to_send = psend.cur_cipher
local pinned_kind, pinned_target = psend.kind, psend.target
local ok, err, err_code = raw_transmit(cipher_to_send, pinned_kind, pinned_target, psend.cur_cipher_offset)
if psend and psend.tid == my_tid and psend.sctr == idx then
psend.last_sent_ms = wada.sys.millis()
psend.last_error_code = ok and nil or err_code
if not ok then
wada.sys.toast("send failed, will retry: " .. tostring(err))
end
end
return true
end
function psend_tick()
if not psend or not psend.last_sent_ms then return end
local required_wait = (psend.last_error_code == "too fast") and RATE_LIMIT_BACKOFF_MS or CHUNK_RETRY_TIMEOUT_MS
if wada.sys.millis() - psend.last_sent_ms < required_wait then return end
if not airtime_clear() then return end
psend.retry_count = (psend.retry_count or 0) + 1
wada.sys.toast(string.format("retrying part %d/%d (attempt %d)", psend.sctr + 1, psend.tot_ch, psend.retry_count))
send_next_chunk()
end
function waiting_enqueue(send_state)
local id = send_state.pad_id
local q = waiting_queue[id]
if q and #q >= WAITING_QUEUE_CAP then
wada.sys.toast(string.format("no path to %s and its mailbox is full (%d waiting already)", send_state.target, #q))
return false
end
if not q then
q = {}
waiting_queue[id] = q
end
table.insert(q, {
cipher_hex = send_state.cipher_hex, kind = send_state.kind, target = send_state.target,
pad_id = id, log_entry = send_state.log_entry, queued_at_ms = wada.sys.millis(),
})
if send_state.log_entry then
send_state.log_entry.kind = "waiting"
end
wada.sys.toast(string.format("no path to %s yet - will keep trying quietly", send_state.target))
return true
end
function flush_waiting(id)
local q = waiting_queue[id]
if not q or #q == 0 then return end
local entry = q[1]
if entry.last_probe_ms and wada.sys.millis() - entry.last_probe_ms < SYNC_PROMPT_MIN_SPACING_MS then return end
if not airtime_clear() then return end
local ok, err, err_code, fingerprint = raw_transmit(entry.cipher_hex, entry.kind, entry.target)
entry.last_probe_ms = wada.sys.millis()
if not ok then return end
table.remove(q, 1)
if #q == 0 then waiting_queue[id] = nil end
if entry.log_entry then
if fingerprint then entry.log_entry.fp = fingerprint end
entry.log_entry.kind = "sent"
refresh_chat_view()
end
wada.sys.toast(string.format("delivered to %s - it was waiting for a path", entry.target))
request_prompt_sync_check(entry.pad_id)
end
local WAITING_LONG_POLL_MS = 10 * 60 * 1000
function waiting_tick()
local now = wada.sys.millis()
for id, q in pairs(waiting_queue) do
if q and #q > 0 then
local entry = q[1]
if not entry.last_probe_ms or now - entry.last_probe_ms >= WAITING_LONG_POLL_MS then
flush_waiting(id)
end
end
end
end
local SSEND_MAX_RETRIES = 5
function ssend_tick()
if not ssend or not ssend.last_sent_ms then return end
local required_wait = (ssend.last_error_code == "too fast") and RATE_LIMIT_BACKOFF_MS or CHUNK_RETRY_TIMEOUT_MS
if wada.sys.millis() - ssend.last_sent_ms < required_wait then return end
if not airtime_clear() then return end
if (ssend.retry_count or 0) >= SSEND_MAX_RETRIES then
local moved = waiting_enqueue(ssend)
if not moved and ssend.log_entry then
ssend.log_entry.kind = "warn"
ssend.log_entry.text = ssend.log_entry.text .. " [send failed]"
end
refresh_chat_view()
ssend = nil
return
end
ssend.retry_count = ssend.retry_count + 1
local sent_ok, sent_err, sent_err_code, fingerprint = raw_transmit(ssend.cipher_hex, ssend.kind, ssend.target, ssend.claim_offset)
ssend.last_sent_ms = wada.sys.millis()
ssend.last_error_code = sent_ok and nil or sent_err_code
if sent_ok then
if ssend.log_entry then
if fingerprint then ssend.log_entry.fp = fingerprint end
ssend.log_entry.kind = "sent"
refresh_chat_view()
end
wada.sys.toast(string.format("sent (attempt %d)", ssend.retry_count))
request_prompt_sync_check(ssend.pad_id)
ssend = nil
else
wada.sys.toast(string.format("retrying (attempt %d): %s", ssend.retry_count, tostring(sent_err)))
end
end
function channel_jitter_tick()
if not pending_channel_send then return end
if wada.sys.millis() < pending_channel_send.fire_at_ms then return end
local job = pending_channel_send
pending_channel_send = nil
local rec = load_pad(job.id, "channel")
if not rec then
if job.log_entry then
job.log_entry.kind = "warn"
job.log_entry.text = job.log_entry.text .. " [send failed - pad no longer available]"
refresh_chat_view()
end
return
end
local need = #job.text
local avail = emmb(rec)
if avail < need then
if job.log_entry then
job.log_entry.kind = "warn"
job.log_entry.text = job.log_entry.text .. string.format(" [send failed - not enough pad: need %d, have %d]", need, avail)
refresh_chat_view()
end
return
end
local key_hex = pks(rec, need, "mine")
local claim_offset = rec.ctr or 0
local ok, cipher_hex, encerr = pcall(otp_encrypt, job.text, key_hex)
if not ok or not cipher_hex then
if job.log_entry then
job.log_entry.kind = "warn"
job.log_entry.text = job.log_entry.text .. " [encrypt error]"
refresh_chat_view()
end
return
end
if not cons_pad(job.id, rec, need, "mine") then
if job.log_entry then
job.log_entry.kind = "warn"
job.log_entry.text = job.log_entry.text .. " [send failed - could not save pad state (" .. tostring(TEST_get_last_cons_pad_error()) .. ")]"
refresh_chat_view()
end
return
end
if airtime_clear() then
if job.log_entry then job.log_entry.kind = "sent" end
refresh_chat_view()
local sent_ok, sent_err, sent_err_code, fingerprint = raw_transmit(cipher_hex, "channel", job.target_name, claim_offset)
if sent_ok then
if job.log_entry and fingerprint then job.log_entry.fp = fingerprint end
wada.sys.toast("sent")
request_prompt_sync_check(job.id)
else
if job.log_entry then
job.log_entry.kind = "retrying"
refresh_chat_view()
end
ssend = {
cipher_hex = cipher_hex, kind = "channel", target = job.target_name, pad_id = job.id,
log_entry = job.log_entry, retry_count = 0, claim_offset = claim_offset,
last_sent_ms = wada.sys.millis(), last_error_code = sent_err_code,
}
wada.sys.toast("send failed, will retry: " .. tostring(sent_err))
end
else
queued_entry, queued_cipher = job.log_entry, cipher_hex
queued_kind, queued_target, queued_id, queued_offset = "channel", job.target_name, job.id, claim_offset
refresh_chat_view()
wada.sys.toast("air busy, holding message")
end
end
function flush_queued_send()
if not queued_cipher then return end
if not airtime_clear() then return end
local sent_ok, sent_err, sent_err_code, fingerprint = raw_transmit(queued_cipher, queued_kind, queued_target, queued_offset)
local sent_id, sent_kind, sent_target, sent_entry, sent_cipher, sent_offset =
queued_id, queued_kind, queued_target, queued_entry, queued_cipher, queued_offset
queued_cipher, queued_entry, queued_kind, queued_target, queued_id, queued_offset = nil, nil, nil, nil, nil, nil
if sent_ok then
if sent_entry then
if fingerprint then sent_entry.fp = fingerprint end
sent_entry.kind = "sent"
end
refresh_chat_view()
wada.sys.toast("sent")
if sent_id then request_prompt_sync_check(sent_id) end
return
end
if sent_entry then
sent_entry.kind = "retrying"
refresh_chat_view()
end
ssend = {
cipher_hex = sent_cipher, kind = sent_kind, target = sent_target, pad_id = sent_id,
log_entry = sent_entry, retry_count = 0, claim_offset = sent_offset,
last_sent_ms = wada.sys.millis(), last_error_code = sent_err_code,
}
wada.sys.toast("send failed, will retry: " .. tostring(sent_err))
end
function complete_send()
if not psend then return end
local pad_id = psend.pad_id
if psend.log_entry then
psend.log_entry.kind = "delivered"
else
a_chat("You", psend.fpay, "delivered")
end
psend = nil
refresh_chat_view()
wada.sys.toast("all parts delivered")
request_prompt_sync_check(pad_id)
end
local chat_logs = {}
local chat_logs_touched_ms = {}
local chat_logs_total_entries = 0
local chat_scroll_offset = 0
local CHAT_SCROLL_STEP = 14

local CHAT_HISTORY_MAX = 150
local MAX_TOTAL_CHAT_ENTRIES = 500
function chat_log_for(id)
local log = chat_logs[id]
if not log then
log = {}
chat_logs[id] = log
end
return log
end
function chat_prune_total()
local protect_id = current_target_id()
while chat_logs_total_entries > MAX_TOTAL_CHAT_ENTRIES do
local oldest_id, oldest_ms = nil, nil
for tid, ms in pairs(chat_logs_touched_ms) do
if tid ~= protect_id then
local log = chat_logs[tid]
if log and #log > 0 and (not oldest_ms or ms < oldest_ms) then
oldest_id, oldest_ms = tid, ms
end
end
end
if not oldest_id then return end
local log = chat_logs[oldest_id]
table.remove(log, 1)
chat_logs_total_entries = chat_logs_total_entries - 1
if #log == 0 then
chat_logs[oldest_id] = nil
chat_logs_touched_ms[oldest_id] = nil
end
end
end
function a_chat(who, text, kind)
local id = current_target_id()
if not id then return nil end
local log = chat_log_for(id)
local entry = { who = who, text = text, kind = kind, at = wada.sys.millis(), at_epoch = now_epoch() }
table.insert(log, entry)
chat_logs_total_entries = chat_logs_total_entries + 1
while #log > CHAT_HISTORY_MAX do
table.remove(log, 1)
chat_logs_total_entries = chat_logs_total_entries - 1
end
chat_logs_touched_ms[id] = wada.sys.millis()
chat_prune_total()
chat_scroll_offset = 0
return entry
end
function a_chat_for(id, who, text, kind)
local log = chat_log_for(id)
local entry = { who = who, text = text, kind = kind, at = wada.sys.millis(), at_epoch = now_epoch() }
table.insert(log, entry)
chat_logs_total_entries = chat_logs_total_entries + 1
while #log > CHAT_HISTORY_MAX do
table.remove(log, 1)
chat_logs_total_entries = chat_logs_total_entries - 1
end
chat_logs_touched_ms[id] = wada.sys.millis()
chat_prune_total()
if id == current_target_id() then chat_scroll_offset = 0 end
return entry
end
function scroll_chat(direction)
chat_scroll_offset = chat_scroll_offset + direction * CHAT_SCROLL_STEP
if current_screen == "chat" then refresh_chat_view() end
end
local SENDER_COLOR_FIELDS = { "decrypt", "ciphertext", "newpad", "keyphrase", "result_ok" }
local sender_colors, next_color_bucket = {}, 1
local SENDER_COLORS_MAX = 200
local sender_colors_order = {}
function color_for_sender(who)
local bucket = sender_colors[who]
if not bucket then
bucket = next_color_bucket
next_color_bucket = (next_color_bucket % #SENDER_COLOR_FIELDS) + 1
sender_colors[who] = bucket
table.insert(sender_colors_order, who)
if #sender_colors_order > SENDER_COLORS_MAX then
local oldest = table.remove(sender_colors_order, 1)
sender_colors[oldest] = nil
end
end
return PAL[SENDER_COLOR_FIELDS[bucket]]
end
function darken_color(hex, factor)
local r = math.floor(((hex >> 16) & 0xFF) * factor)
local g = math.floor(((hex >> 8) & 0xFF) * factor)
local b = math.floor((hex & 0xFF) * factor)
return (r << 16) | (g << 8) | b
end
function lighten_color(hex, t)
local r, g, b = (hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF
r = math.floor(r + (255 - r) * t)
g = math.floor(g + (255 - g) * t)
b = math.floor(b + (255 - b) * t)
return (r << 16) | (g << 8) | b
end
function contrast_of(col)
local function lin(c)
c = c / 255
return c <= 0.03928 and c / 12.92 or ((c + 0.055) / 1.055) ^ 2.4
end
local function lum(h)
return 0.2126 * lin((h >> 16) & 0xFF) + 0.7152 * lin((h >> 8) & 0xFF) + 0.0722 * lin(h & 0xFF)
end
local l = lum(col) + 0.05
if 0.96 / l >= 4.5 then return 0xF5F5F5 end
local shade = darken_color(col, 0.2)
if l / (lum(shade) + 0.05) >= 4.5 then return shade end
return 0x14171c
end
function draw_round_rect(cv, x, y, w, h, r, color)
r = math.min(r, math.floor(w / 2), math.floor(h / 2))
cv:rect(x, y, w, h, color, true, r)
end
function wrap_text_lines(text, max_width, size)
if text == "" then return { "" } end
if type(wada.ui.text_w) ~= "function" then
local lines = {}
local chars_per_line = 24
for i = 1, #text, chars_per_line do
table.insert(lines, text:sub(i, i + chars_per_line - 1))
end
return lines
end
local SAFE_CHUNK_CHARS = 100
local function split_long_word(word, mw, sz)
local chunks = {}
local i = 1
while i <= #word do
local remaining = #word - i + 1
local lo, hi, best = 1, remaining, 1
while lo <= hi do
local mid = math.floor((lo + hi) / 2)
local ok, cw = pcall(wada.ui.text_w, word:sub(i, i + mid - 1), sz)
if ok and type(cw) == "number" and cw <= mw then
best = mid
lo = mid + 1
else
hi = mid - 1
end
end
table.insert(chunks, word:sub(i, i + best - 1))
i = i + best
end
return chunks
end
local lines = {}
local current = ""
for raw_word in text:gmatch("%S+") do
for pre_start = 1, #raw_word, SAFE_CHUNK_CHARS do
local word = raw_word:sub(pre_start, pre_start + SAFE_CHUNK_CHARS - 1)
local ok_w, ww = pcall(wada.ui.text_w, word, size)
local word_chunks
if ok_w and type(ww) == "number" and ww > max_width then
word_chunks = split_long_word(word, max_width, size)
else
word_chunks = { word }
end
for _, chunk in ipairs(word_chunks) do
local candidate = (current == "") and chunk or (current .. " " .. chunk)
local ok, w = pcall(wada.ui.text_w, candidate, size)
if ok and type(w) == "number" and w > max_width and current ~= "" then
table.insert(lines, current)
current = chunk
else
current = candidate
end
end
end
end
if current ~= "" then table.insert(lines, current) end
if #lines == 0 then lines = { "" } end
return lines
end
local BUBBLE_PAD = 6
local BUBBLE_GAP = 6
local BUBBLE_MAX_W_FRAC = 0.72
function chat_bg_tint()
return PAL.bg
end
local SIG_RED, SIG_ORANGE, SIG_YELLOW, SIG_GREEN, SIG_GREEN_BRIGHT = 0xE74C3C, 0xE67E22, 0xF1C40F, 0x2ECC71, 0x39FF14
local SIGNAL_SAMPLE_INTERVAL_MS = 3000
local SIGNAL_HISTORY_LEN = 8
local sig_history, sig_history_idx, sig_history_count, sig_history_sum = {}, 0, 0, 0
local sig_grade, sig_color = "no data", SIG_RED
local next_signal_sample_ms = 0
function sig_grade_for_margin(margin)
if margin >= 20 then return "excellent", SIG_GREEN_BRIGHT
elseif margin >= 12 then return "good", SIG_GREEN
elseif margin >= 6 then return "fair", SIG_YELLOW
elseif margin >= 0 then return "poor", SIG_ORANGE
else return "very poor", SIG_RED
end
end
function sample_signal_if_due()
local now = wada.sys.millis()
if now < next_signal_sample_ms then return false end
next_signal_sample_ms = now + SIGNAL_SAMPLE_INTERVAL_MS
if type(wada.mesh.stats) ~= "function" then
local before = sig_grade
sig_grade, sig_color = "no data", SIG_RED
return sig_grade ~= before
end
local ok, stats = pcall(wada.mesh.stats)
local before = sig_grade
if not ok or type(stats) ~= "table" or type(stats.rssi) ~= "number" or type(stats.noise) ~= "number" then
sig_grade, sig_color = "no data", SIG_RED
return sig_grade ~= before
end
local margin = stats.rssi - stats.noise
sig_history_idx = (sig_history_idx % SIGNAL_HISTORY_LEN) + 1
if sig_history[sig_history_idx] ~= nil then
sig_history_sum = sig_history_sum - sig_history[sig_history_idx]
else
sig_history_count = sig_history_count + 1
end
sig_history[sig_history_idx] = margin
sig_history_sum = sig_history_sum + margin
sig_grade, sig_color = sig_grade_for_margin(sig_history_sum / sig_history_count)
return sig_grade ~= before
end
function signal_tick()
if current_screen ~= "chat" then return end
if sample_signal_if_due() then refresh_chat_bytes() end
end
local CHAT_LOW_PAD_BYTES = 100
function refresh_chat_bytes()
if not W.chat_bytes_cv then return end
local rec, id = current_pad()
local text, color = nil, PAL.padinfo
local badge = id and resync_badge[id]
if badge and wada.sys.millis() < badge.until_ms then
text, color = badge.text, PAL.result_ok
elseif not rec or not id then
text = "no pad"
else
local bytes = emmb(rec)
text = string.format("%d bytes available", bytes)
if bytes <= 0 then
text = text .. " - pad exhausted"
elseif bytes < CHAT_LOW_PAD_BYTES then
text = text .. " - running low"
end
end
pcall(function()
W.chat_bytes_cv:fill(chat_bg_tint())
W.chat_bytes_cv:text(0, 0, text, color, 10)
local sig_text = "Signal: " .. sig_grade
local ok_sw, sig_tw = pcall(wada.ui.text_w, sig_text, 9)
local BAR_W, BAR_GAP, BAR_COUNT = 2, 1, 4
local bars_w = BAR_COUNT * BAR_W + (BAR_COUNT - 1) * BAR_GAP
local row_w = W.chat_bytes_w or 0
local bars_x = row_w - bars_w
local sig_x = bars_x - 4 - (ok_sw and sig_tw or 60)
W.chat_bytes_cv:text(sig_x, 0, sig_text, sig_color, 9)
local lit = 0
if sig_grade == "excellent" then lit = 4
elseif sig_grade == "good" then lit = 3
elseif sig_grade == "fair" then lit = 2
elseif sig_grade == "poor" then lit = 1
end
local baseline = (W.chat_bytes_h or 10) - 2
for i = 1, BAR_COUNT do
local bh = i + 2
local bx = bars_x + (i - 1) * (BAR_W + BAR_GAP)
local by = baseline - bh
local col = (i <= lit) and sig_color or PAL.ready
W.chat_bytes_cv:rect(bx, by, BAR_W, bh, col)
end
end)
end
function refresh_participant_strip()
if not W.participant_cv then return end
local id = current_target_id()
local participants = id and active_channel_participants(id) or {}
pcall(function()
W.participant_cv:fill(chat_bg_tint())
if #participants == 0 then W.participant_cv:text(0, 0, "no one heard yet", PAL.ready, 9) end
local x = 0
local size = 9
for _, p in ipairs(participants) do
local color = (p.in_sync == true) and PAL.result_ok or PAL.ready
W.participant_cv:text(x, 0, p.name, color, size)
local ok, tw = pcall(wada.ui.text_w, p.name, size)
x = x + (ok and type(tw) == "number" and tw or (#p.name * 6)) + 10
end
end)
end
function relative_time_str(now_ms, at_ms)
if type(at_ms) ~= "number" then return "" end
local diff_s = math.floor((now_ms - at_ms) / 1000)
if diff_s < 0 then diff_s = 0 end
if diff_s < 5 then return "just now" end
if diff_s < 60 then return diff_s .. "s ago" end
local diff_m = math.floor(diff_s / 60)
if diff_m < 60 then return diff_m .. "m ago" end
local diff_h = math.floor(diff_m / 60)
if diff_h < 24 then return diff_h .. "h ago" end
return math.floor(diff_h / 24) .. "d ago"
end

function now_epoch()
local ok, t = pcall(function() return wada.sys.epoch and wada.sys.epoch() end)
if ok and type(t) == "number" and t > 0 then return t end
return nil
end

function civil_from_epoch(epoch_s)
local days = math.floor(epoch_s / 86400)
local secs_of_day = epoch_s - days * 86400
local hour = math.floor(secs_of_day / 3600)
local minute = math.floor((secs_of_day % 3600) / 60)
local z = days + 719468
local era
if z >= 0 then era = math.floor(z / 146097) else era = math.floor((z - 146096) / 146097) end
local doe = z - era * 146097
local yoe = math.floor((doe - math.floor(doe / 1460) + math.floor(doe / 36524) - math.floor(doe / 146096)) / 365)
local y = yoe + era * 400
local doy = doe - (365 * yoe + math.floor(yoe / 4) - math.floor(yoe / 100))
local mp = math.floor((5 * doy + 2) / 153)
local d = doy - math.floor((153 * mp + 2) / 5) + 1
local m
if mp < 10 then m = mp + 3 else m = mp - 9 end
if m <= 2 then y = y + 1 end
return y, m, d, hour, minute
end
local SHORT_MONTH_NAMES = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" }

function wall_clock_str(at_epoch, now_epoch_val)
if type(at_epoch) ~= "number" then return nil end
local y, m, d, h, mi = civil_from_epoch(at_epoch)
local time_part = string.format("%02d:%02d", h, mi)
if type(now_epoch_val) == "number" then
local ny, nm, nd = civil_from_epoch(now_epoch_val)
if ny == y and nm == m and nd == d then return time_part end
end
return string.format("%s %d, %s", SHORT_MONTH_NAMES[m] or "?", d, time_part)
end

function entry_time_str(now_ms, now_epoch_val, e)
local wall = wall_clock_str(e.at_epoch, now_epoch_val)
if wall then return wall end
return relative_time_str(now_ms, e.at)
end
function status_word_for(e)
if e.kind == "waiting" then return "Waiting for a path" end
if e.kind == "pending" then
if psend and psend.log_entry == e and psend.tot_ch and psend.tot_ch > 1 then
return string.format("Sending %d/%d...", psend.sctr + 1, psend.tot_ch)
end
return "Sending..."
elseif e.kind == "retrying" then
if ssend and ssend.log_entry == e and ssend.retry_count then
return string.format("Retrying (%d/%d)...", ssend.retry_count, SSEND_MAX_RETRIES)
end
return "Retrying..."
elseif e.kind == "failed" then
return "Failed: " .. (FAIL_WORDS[e.fail] or "?") .. ((dkind == "channel" and e.fail_who) and (" (" .. tostring(e.fail_who):sub(1, 6) .. ")") or "")
elseif e.kind == "warn" then return "Lost"
elseif e.kind == "delivered" then return "Delivered"
elseif e.kind == "sent" then
if e.rep and e.rep > 0 then return "Heard x" .. e.rep end
return "Sent"
end
return ""
end
function refresh_chat_view()
if not W.chat_cv then return end
refresh_chat_bytes()
refresh_participant_strip()
local id = current_target_id()
local log = id and chat_log_for(id) or {}
pcall(function()
W.chat_cv:fill(chat_bg_tint())
local size = 12
local line_h = math.floor(wada.ui.text_h(size))
local caption_size = 9
local caption_h = math.floor(wada.ui.text_h(caption_size)) + 2
local canvas_w = W.chat_w or 200
local max_bubble_w = math.floor(canvas_w * BUBBLE_MAX_W_FRAC)
local now_ms = wada.sys.millis()
local now_epoch_val = now_epoch()

local sent_on_left = otp_active_for_current_chat()
local computed = {}
local total_h = 0
for i = #log, 1, -1 do
local e = log[i]
local is_own_warn = e.kind == "warn" and e.who == "You"
local is_sent = e.kind == "sent" or e.kind == "delivered" or e.kind == "pending" or e.kind == "retrying" or e.kind == "waiting" or e.kind == "failed" or is_own_warn
local is_received = e.kind == "received"
local full_text = e.text
local caption = ""
if is_received then
caption = string.format("%s · %s", (type(e.who) == "string" and e.who ~= "") and e.who or "Them", entry_time_str(now_ms, now_epoch_val, e))
elseif is_sent then
local status = status_word_for(e)
local tstr = entry_time_str(now_ms, now_epoch_val, e)
caption = status ~= "" and string.format("%s · %s", status, tstr) or tstr
end
local lines, bubble_w, bubble_h
if e._cache_text == full_text and e._cache_lines then
lines = e._cache_lines
bubble_w = e._cache_bubble_w
bubble_h = e._cache_bubble_h
else
lines = wrap_text_lines(full_text, max_bubble_w - 2 * BUBBLE_PAD, size)
local text_w = 0
for _, ln in ipairs(lines) do
local ok, w = pcall(wada.ui.text_w, ln, size)
if ok and type(w) == "number" and w > text_w then text_w = w end
end
bubble_w = math.min(max_bubble_w, text_w + 2 * BUBBLE_PAD)
bubble_h = #lines * line_h + 2 * BUBBLE_PAD
e._cache_text = full_text
e._cache_lines = lines
e._cache_bubble_w = bubble_w
e._cache_bubble_h = bubble_h
end
local ok_capw, cap_w = pcall(wada.ui.text_w, caption, caption_size)
if not ok_capw or type(cap_w) ~= "number" then cap_w = 0 end
local unit_w = math.max(bubble_w, math.min(max_bubble_w, cap_w))
table.insert(computed, {
e = e, is_sent = is_sent, is_received = is_received,
lines = lines, bubble_w = bubble_w, bubble_h = bubble_h,
caption = caption, caption_w = cap_w, unit_w = unit_w,
})
total_h = total_h + caption_h + bubble_h + BUBBLE_GAP
if total_h > W.chat_h + chat_scroll_offset + W.chat_h then break end
end
local keep = {}
for _, c in ipairs(computed) do keep[c.e] = true end
for _, e in ipairs(log) do
if not keep[e] and e._cache_lines then
e._cache_text = nil; e._cache_lines = nil; e._cache_bubble_w = nil; e._cache_bubble_h = nil
end
end
local max_scroll = math.max(0, total_h - W.chat_h)
if chat_scroll_offset > max_scroll then chat_scroll_offset = max_scroll end
if chat_scroll_offset < 0 then chat_scroll_offset = 0 end
local y_bottom = W.chat_h + chat_scroll_offset
for _, c in ipairs(computed) do
local e, bubble_w, bubble_h, lines = c.e, c.bubble_w, c.bubble_h, c.lines
local is_sent, is_received = c.is_sent, c.is_received
local by = y_bottom - caption_h - bubble_h
local cap_y = by + bubble_h + 2
if by + bubble_h + caption_h < 0 then break end
if by < W.chat_h then

local bubble_on_left
if is_sent then
bubble_on_left = sent_on_left
elseif is_received then
bubble_on_left = not sent_on_left
end
local bx
if bubble_on_left == true then
bx = 2
elseif bubble_on_left == false then
bx = canvas_w - bubble_w - 2
else
bx = math.floor((canvas_w - bubble_w) / 2)
end
if c.caption ~= "" then
local cap_x
if bubble_on_left == true then
cap_x = 2
elseif bubble_on_left == false then
cap_x = math.max(2, canvas_w - c.caption_w - 2)
else
cap_x = 2
end
W.chat_cv:text(cap_x, cap_y, c.caption, PAL.padinfo, caption_size)
end
local outline_color = PAL.plaintext
if e.kind == "sent" or e.kind == "pending" then outline_color = PAL.channel
elseif e.kind == "delivered" then outline_color = PAL.result_ok
elseif e.kind == "retrying" then outline_color = PAL.keyphrase
elseif e.kind == "waiting" then outline_color = PAL.ready
elseif is_received then outline_color = color_for_sender(e.who or "Them")
elseif e.kind == "warn" or e.kind == "failed" then outline_color = PAL.encrypt
end
local fill_color = (e.kind == "pending" or e.kind == "retrying") and darken_color(outline_color, 0.6) or outline_color
local text_color = contrast_of(fill_color)
local br = math.min(5, BUBBLE_PAD - 1)
draw_round_rect(W.chat_cv, bx, by, bubble_w, bubble_h, br, lighten_color(outline_color, 0.35))
draw_round_rect(W.chat_cv, bx + 1, by + 1, bubble_w - 2, bubble_h - 2, br - 1, fill_color)
for li, ln in ipairs(lines) do
W.chat_cv:text(bx + BUBBLE_PAD, by + BUBBLE_PAD + (li - 1) * line_h, ln, text_color, size)
end
end
y_bottom = by - BUBBLE_GAP
end
if max_scroll > 0 then
local scrollbar_w = 3
local track_x = canvas_w - scrollbar_w
W.chat_cv:rect(track_x, 0, scrollbar_w, W.chat_h, darken_color(PAL.channel, 0.4), true, 0)
local visible_frac = W.chat_h / (total_h > 0 and total_h or W.chat_h)
local thumb_h = math.max(6, math.floor(W.chat_h * visible_frac))
local thumb_travel = W.chat_h - thumb_h
local thumb_y = math.floor(thumb_travel * chat_scroll_offset / max_scroll)
W.chat_cv:rect(track_x, thumb_y, scrollbar_w, thumb_h, PAL.channel, true, 0)
end
end)
end
local RECENT_WIRE_TEXT_CAP = 6
local rwtexts = {}
function is_dup_wt(text)
for _, seen in ipairs(rwtexts) do
if seen == text then return true end
end
return false
end
function remember_wire_text(text)
table.insert(rwtexts, text)
if #rwtexts > RECENT_WIRE_TEXT_CAP then
table.remove(rwtexts, 1)
end
end
local RECENT_MSG_CACHE_SIZE, RECENT_MSG_WINDOW_MS = 8, 3000
local rmc, rmcn = {}, 1
function check_and_remember(key)
local now = wada.sys.millis()
for i = 1, #rmc do
local entry = rmc[i]
if entry.key == key and (now - entry.at) < RECENT_MSG_WINDOW_MS then
return true
end
end
rmc[rmcn] = { key = key, at = now }
rmcn = (rmcn % RECENT_MSG_CACHE_SIZE) + 1
return false
end
local function ascii_core(s)
if type(s) ~= "string" then return s end
local i = #s
while i > 0 do
local b = s:byte(i)
if b >= 0x80 or b == 0x20 then i = i - 1 else break end
end
return s:sub(1, i):lower()
end
function find_contact_by_name(name)

local contacts = p_cts()
for _, c in ipairs(contacts) do
if c.name == name then return c end
end

if type(name) == "string" and name:match("^[0-9a-fA-F]+$") then
local lname = name:lower()
for _, c in ipairs(contacts) do

if type(c.pubkey) == "string" and c.pubkey ~= "" and c.pubkey:lower() == lname then return c end
if type(c.name) == "string" and c.name:lower() == lname then return c end
end
end

if type(name) == "string" and name ~= "" then
local best, best_len = nil, -1
for _, c in ipairs(contacts) do
if type(c.name) == "string" and #c.name > #name and c.name:sub(1, #name) == name and #c.name > best_len then
best, best_len = c, #c.name
end
end
if best then return best end
end

if type(name) == "string" and name ~= "" then
local core = ascii_core(name)
if core ~= "" then
for _, c in ipairs(contacts) do
if type(c.name) == "string" and ascii_core(c.name) == core then return c end
end
end
end
return nil
end

local REASSEMBLY_RETRY_BACKOFF_MS = 1800
local REASSEMBLY_RETRY_MAX = 6
local pending_reassembly = {} 
function finish_reassembly(id, rec, kind, sender, imt)
local parts = {}
for i = 0, imt.tot_ch - 1 do parts[#parts + 1] = imt.chunks[i] end
local ok, joined = pcall(table.concat, parts)
if not ok then return false, joined end
imt.completed = true
imt.chunks = nil
if llg(joined) then
REASM_GARBAGE = true
a_chat_for(id, sender, "[reassembled garbage - check the pad is shared correctly]", "warn")
wada.sys.toast("garbage/desync from " .. sender .. " (" .. pad_debug_str(rec, kind) .. ")")
request_prompt_sync_check(id)
return true
end
a_chat_for(id, sender, joined, "received")
if current_screen == "chat" and current_target_id() == id then refresh_chat_view() end
play_notify_sound()
wada.sys.toast("message complete from " .. sender)
request_prompt_sync_check(id)
return true
end
function reassembly_retry_tick()
for id, p in pairs(pending_reassembly) do
if wada.sys.millis() - p.last_attempt_ms >= REASSEMBLY_RETRY_BACKOFF_MS then
p.retry_count = p.retry_count + 1
p.last_attempt_ms = wada.sys.millis()
local imt = imt_by_target[id]
if not imt or imt.tid ~= p.tid or imt.completed or not imt.chunks then
pending_reassembly[id] = nil
else
local rec = load_pad(id, p.kind)
local ok = finish_reassembly(id, rec, p.kind, p.sender, imt)
if ok then
pending_reassembly[id] = nil
elseif p.retry_count >= REASSEMBLY_RETRY_MAX then
pending_reassembly[id] = nil

imt_by_target[id] = nil
wada.sys.toast("could not reassemble message from " .. tostring(p.sender) .. " after retrying - it will not arrive")
else
wada.sys.toast(string.format("still low on memory, retrying reassembly (%d/%d)...", p.retry_count, REASSEMBLY_RETRY_MAX))
end
end
end
end
end
function do_on_message(m)
if type(m) ~= "table" then
wada.sys.toast("on_message: non-table payload")
return
end
local text = m.text or m.body or m.msg or m.message or m.content
local ch = m.channel or m.chan or m.chname
if type(text) ~= "string" or text == "" then return end
local kind, sender, id, nack_target
local dm_contact_found = nil
local dm_pubkey_hint = nil
if m.kind == "dm" or m.kind == "room" then
kind = "dm"
sender = (type(m.sender) == "string" and m.sender ~= "") and m.sender or "Them"
local c = find_contact_by_name(sender)
nack_target = c and c.name or sender
if c == nil and #p_cts() == 0 then
wada.sys.toast("contact list empty (known SDK issue) - power-cycle the device to fix. Ignoring message from " .. sender)
return
end
dm_contact_found = (c ~= nil)
dm_pubkey_hint = c and (type(c.pubkey) == "string" and c.pubkey ~= "" and c.pubkey:sub(1, 8) or "EMPTY") or "NOCTC"
id = target_id("dm", c and contact_key(c) or sender)
elseif m.kind == "channel" then
kind = "channel"
local chan_name = normalize_channel(ch)
nack_target = chan_name
sender = (type(m.sender) == "string" and m.sender ~= "") and m.sender or nil
local colon_at = text:find(": ", 1, true)
if colon_at then
local candidate = text:sub(colon_at + 2)
if candidate ~= "" and candidate:match("^[0-9a-fA-F]+$") and #candidate % 2 == 0 then
local raw_name = text:sub(1, colon_at - 1)
if raw_name ~= "" then sender = raw_name end
text = candidate
end
end
sender = sender or "Them"
id = target_id("channel", chan_name)
if is_public_channel_name(chan_name) then return end
else
return
end
if text:sub(1, #SYNC_MARKER) == SYNC_MARKER then
handle_sync_message(text, sender, kind, id)
return
end
if text:find("z4%uz$") then
handle_nack(text, sender, id)
return
end
local claimed_channel_offset = nil
if kind == "channel" and text:sub(1, #CHANNEL_OFFSET_MARKER) == CHANNEL_OFFSET_MARKER then
local off_str, cipher_part = text:sub(#CHANNEL_OFFSET_MARKER + 1):match("^([0-9a-zA-Z]+)|(.+)$")
if not off_str then return end
local off_val = from_base36(off_str)
if not off_val then return end
claimed_channel_offset = off_val
text = cipher_part
end
if kind == "channel" then
local heard_rec = load_pad(id, kind)
record_channel_participant_heard(id, sender, claimed_channel_offset, heard_rec and heard_rec.ctr or 0)
if current_screen == "chat" and current_target_id() == id then refresh_participant_strip() end
end
if not text:match("^[0-9a-fA-F]+$") or #text % 2 ~= 0 then
local hex_tail = text:match("([0-9a-fA-F]+)$")
if not hex_tail or hex_tail == "" or #hex_tail % 2 ~= 0 then return end
text = hex_tail
end

local function fail_nack(reason)
queue_nack(kind, nack_target, reason)
end
local rec = load_pad(id, kind)
local imt = imt_by_target[id]
if imt and imt.raw_wire_by_idx then
for known_idx, known_text in pairs(imt.raw_wire_by_idx) do
if known_text == text then
send_ack_for_chunk(id, rec, imt.tid, known_idx, sender)
return
end
end
end
if check_and_remember(id .. "|" .. text) then return end
if is_dup_wt(text) then return end
local cbytes = math.floor(#text / 2)
local avail = emmb(rec)
if avail < cbytes then
return
end
if avail > MAX_TOTAL_PAD_BYTES then
wada.sys.toast(string.format("pad for %s too large to process safely (%d bytes) - wipe it to continue receiving", sender, avail))
return
end
remember_wire_text(text)
if claimed_channel_offset ~= nil then
local current_ctr = rec.ctr or 0
if claimed_channel_offset < current_ctr then
a_chat_for(id, sender, "[channel message lost - collided with another sender]", "warn")
if current_screen == "chat" and current_target_id() == id then refresh_chat_view() end
wada.sys.toast("channel collision from " .. sender)
fail_nack("C")
request_prompt_sync_check(id)
send_sync_check(id, kind, rec, false)
return
elseif claimed_channel_offset > current_ctr then
local pending = pending_resync[id]
if not (pending and pending.peer_ctr == claimed_channel_offset and pending.kind == "channel") then
pending_resync[id] = { peer_ctr = claimed_channel_offset, kind = "channel", first_seen_ms = wada.sys.millis() }
end
return
end
end
local key_hex = pks(rec, cbytes, "theirs")
local ok, plaintext = pcall(otp_decrypt, text, key_hex)
if not ok or not plaintext then
wada.sys.toast("decrypt error from " .. sender)
fail_nack("D")
return
end
if not cons_pad(id, rec, cbytes, "theirs") then
wada.sys.toast("could not save pad state (" .. tostring(TEST_get_last_cons_pad_error()) .. ")")
return
end
if plaintext:sub(1, #MARKER.msg_ack) == MARKER.msg_ack then
local rest = plaintext:sub(#MARKER.msg_ack + 1)
if #rest < 2 then return end
local acked_tid, acked_idx = string.byte(rest, 1), string.byte(rest, 2)

if psend and psend.pad_id == id and psend.tid == acked_tid and acked_idx == psend.sctr then
psend.sctr = psend.sctr + 1
if psend.sctr >= psend.tot_ch then
complete_send()
else
send_next_chunk()
refresh_chat_view()
end
else
wada.sys.toast("ack mismatch")
end
return
end
if plaintext:sub(1, #MARKER.msg_chunk) == MARKER.msg_chunk then
local rest = plaintext:sub(#MARKER.msg_chunk + 1)
if #rest < 3 then
wada.sys.toast("bad chunk from " .. sender)
return
end
local tid, c_idx, tot_ch = string.byte(rest, 1), string.byte(rest, 2), string.byte(rest, 3)
local chunk_data = rest:sub(4)
if chunk_data == "" or tot_ch == 0 then
wada.sys.toast("empty chunk from " .. sender)
return
end
if not imt or imt.tid ~= tid then
imt = { tid = tid, chunks = {}, tot_ch = tot_ch, sender = sender, raw_wire_by_idx = {} }
imt_by_target[id] = imt
end
if imt.completed or imt.chunks[c_idx] ~= nil then
send_ack_for_chunk(id, rec, tid, c_idx, sender)
return
end
imt.chunks[c_idx] = chunk_data
imt.raw_wire_by_idx[c_idx] = text
local have = 0
for i = 0, tot_ch - 1 do
if imt.chunks[i] then have = have + 1 end
end
local ack_ok, ack_err = send_ack_for_chunk(id, rec, tid, c_idx, sender)
if not ack_ok then
wada.sys.toast("ack failed: " .. tostring(ack_err))
end
if have >= tot_ch then
local pok, ok, reassemble_err = pcall(finish_reassembly, id, rec, kind, sender, imt)
if not pok then reassemble_err = ok; ok = false end
if REASM_GARBAGE then
REASM_GARBAGE = false
fail_nack("G")
end
if not ok then
pending_reassembly[id] = { tid = tid, kind = kind, sender = sender, retry_count = 0, last_attempt_ms = wada.sys.millis() }
wada.sys.toast("reassembly error, will retry: " .. tostring(reassemble_err))
end
else
do end
end
return
end
if llg(plaintext) then
a_chat_for(id, sender, "[decrypted garbage - check the pad is shared correctly]", "warn")
wada.sys.toast("garbage/desync from " .. sender .. " (" .. pad_debug_str(rec, kind) .. ")")
request_prompt_sync_check(id)
fail_nack("G")
return
end
a_chat_for(id, sender, plaintext, "received")
if current_screen == "chat" and current_target_id() == id then refresh_chat_view() end
play_notify_sound()
request_prompt_sync_check(id)
end
function app.on_message(m)
local ok, err = pcall(do_on_message, m)
pcall(trim_pad_cache)
if not ok then
if tostring(err):find("not enough memory", 1, true) then
for pid in pairs(pads_cache) do
if not pending_pad_saves[pid] then pads_cache[pid] = nil end
end
pads_cache_mru = {}
end
wada.sys.toast("message handling error: " .. clean_err(err))
end
end
local snd_on = true
function play_notify_sound()
if not snd_on then return end
if type(wada.sys.beep) == "function" then
pcall(wada.sys.beep)
end
end
function remaining_message_chars()
local cap = (dkind == "channel") and multi_wire_cap() or max_singleshot_bytes()
return math.max(0, cap - #compose_buffer)
end
function compose_visible_text(available_w)
if compose_buffer == "" then return "(empty)" end
local size = 12
if not (wada.ui.text_w) then
local approx_chars = math.max(1, math.floor(available_w / 6))
if #compose_buffer <= approx_chars then return compose_buffer end
return compose_buffer:sub(#compose_buffer - approx_chars + 1)
end
local max_possible_chars = math.max(4, math.floor(available_w / 3))
local probe = compose_buffer
local probe_is_full_buffer = true
if #probe > max_possible_chars then
probe = probe:sub(#probe - max_possible_chars + 1)
probe_is_full_buffer = false
end
local ok_full, full_w = pcall(wada.ui.text_w, probe, size)
if ok_full and probe_is_full_buffer and full_w <= available_w then return compose_buffer end
local n = #probe
for start = n, 1, -1 do
local suffix = probe:sub(start)
local ok, w = pcall(wada.ui.text_w, suffix, size)
if not ok then return suffix end
if w > available_w then
return probe:sub(start + 1)
end
end
return probe
end
function compose_counter_text()
local rec = current_pad()
local avail = rec and emmb(rec) or 0
local base = string.format("%d | %d/%d", remaining_message_chars(), #compose_buffer, avail)
if dkind == "dm" then
local threshold = max_singleshot_bytes()
if #compose_buffer > threshold then
local ccap = max_chunk_bytes()
local n_chunks = math.max(1, math.ceil(#compose_buffer / ccap))
base = base .. string.format(" (%d chunks)", n_chunks)
end
end
return base
end
function open_compose_input()
chat_compose_active = true
compose_buffer = ""
goto_screen("chat")
request_prompt_sync_check(current_target_id())
end
function compose_send()
local text = compose_buffer
if text == "" then
chat_compose_active = false
compose_buffer = ""
goto_screen("chat")
return
end
local ok = send_chat_message(text)
if ok then
chat_compose_active = false
compose_buffer = ""
goto_screen("chat")
end
end
function update_compose_preview_and_counter()
if W.compose_preview then W.compose_preview:set(compose_visible_text(W.compose_preview_w or 150)) end
if W.compose_counter then
W.compose_counter:set(compose_counter_text())
if W.compose_counter.color then
local rec = current_pad()
local avail = rec and emmb(rec) or 0
W.compose_counter:color(#compose_buffer > avail and PAL.encrypt or PAL.padinfo)
end
end
end
function compose_del()
if compose_buffer == "" then
compose_cancel()
return
end
compose_buffer = compose_buffer:sub(1, #compose_buffer - 1)
update_compose_preview_and_counter()
end
function compose_cancel()
chat_compose_active = false
compose_buffer = ""
goto_screen("chat")
end
function chat_compose_handle_input(ev)
if not chat_compose_active then return false end
if (ev.type == "dir" or ev.type == "swipe") and ev.dir == "select" then
compose_send()
return true
end
if ev.type ~= "key" then return false end
if ev.key == "enter" then
compose_send()
elseif ev.key == "esc" then
compose_cancel()
elseif ev.key == "backspace" then
compose_del()
elseif type(ev.key) == "string" and #ev.key == 1 then
if dkind == "channel" and #compose_buffer >= multi_wire_cap() then
wada.sys.toast("limit reached for a channel message - send, or DM instead for a longer message")
else
compose_buffer = compose_buffer .. ev.key
update_compose_preview_and_counter()
end
end
return true
end
function refresh_send_progress()
if not W.send_progress_cv then return end
if not psend then

pcall(function() W.send_progress_cv:fill(chat_bg_tint()) end)
W.send_progress_cv = nil
return
end
pcall(function()
local bw = W.send_progress_w or 100
local bh = W.send_progress_h or 6
W.send_progress_cv:fill(darken_color(PAL.channel, 0.25))
local frac = psend.tot_ch > 0 and (psend.sctr / psend.tot_ch) or 0
local fill_w = math.max(0, math.min(bw, math.floor(bw * frac)))
if fill_w > 0 then
W.send_progress_cv:rect(0, 0, fill_w, bh, PAL.channel)
end
end)
end
function refresh_recv_progress()
if not W.recv_progress_cv then return end
local id = current_target_id()
local imt = id and imt_by_target[id]
pcall(function()
if not imt or imt.completed or not imt.tot_ch or imt.tot_ch <= 0 then
W.recv_progress_cv:fill(PAL.bg)
return
end
local have = 0
for i = 0, imt.tot_ch - 1 do
if imt.chunks[i] then have = have + 1 end
end
local bw = W.recv_progress_w or 100
local bh = W.recv_progress_h or 14
W.recv_progress_cv:fill(darken_color(PAL.result_ok, 0.2))
local frac = have / imt.tot_ch
local fill_w = math.max(0, math.min(bw, math.floor(bw * frac)))
if fill_w > 0 then
W.recv_progress_cv:rect(0, 0, fill_w, bh, PAL.result_ok)
end
local label = string.format("Receiving: %d/%d", have, imt.tot_ch)
W.recv_progress_cv:text(2, 1, label, contrast_of(darken_color(PAL.result_ok, 0.2)), 10)
end)
end
function refresh_pad_status()
refresh_pad_info()
end
function refresh_pad_sync_label()
refresh_pad_info()
end
function refresh_pad_detail_label()
refresh_pad_info()
end
function refresh_pad_info()
if not W.pad_info_cv then return end
local rec, id = current_pad()
local bytes = rec and emmb(rec) or 0
local over_cap = bytes > MAX_TOTAL_PAD_BYTES
local status_color = over_cap and PAL.encrypt or PAL.padinfo
local status_text
if not rec or not id then
status_text = "no target selected"
elseif over_cap then
status_text = string.format("%d bytes - over the %d cap, wipe recommended", bytes, MAX_TOTAL_PAD_BYTES)
else
status_text = string.format("%d bytes available", bytes)
end
local sync = id and sync_status_by_target[id]
local sync_text = sync and sync.text or "not checked yet"
local sync_color = PAL.padinfo
if sync and sync.ok ~= nil then
sync_color = sync.ok and PAL.result_ok or PAL.encrypt
end
local detail_text
if not rec or not id then
detail_text = ""
elseif dkind == "dm" then
detail_text = string.format("mine=%d theirs=%d bytes=%d", rec.ctr_mine or 0, rec.ctr_theirs or 0, emmb(rec))
else
detail_text = string.format("ctr=%d bytes=%d", rec.ctr or 0, emmb(rec))
end
local pw = W.pad_info_w or 100
local status_h = 18
pcall(function()
W.pad_info_cv:fill(PAL.bg)
W.pad_info_cv:rect(0, 0, pw, status_h, darken_color(status_color, 0.55), true, 3)
W.pad_info_cv:rect(0, 0, pw, status_h, status_color, false, 3)
W.pad_info_cv:text(6, math.floor((status_h - 10) / 2), status_text, contrast_of(darken_color(status_color, 0.55)), 10)
W.pad_info_cv:text(0, status_h + 2, "Sync: " .. sync_text, sync_color, 10)
W.pad_info_cv:text(0, status_h + 16, detail_text, PAL.padinfo, 10)
end)
end
function commit_pad_text_bytes(id, kind, text)
if kind == "channel" and is_public_channel_id(id) then
wada.sys.toast("OTP is not available on the Public channel")
return false
end
if text == "" then
wada.sys.toast("nothing typed")
return false
end
if #text > MXADD then
wada.sys.toast(string.format("%d bytes is too much for one add (max %d)", #text, MXADD))
return false
end
local rec = load_pad(id, kind)
if emmb(rec) + #text > MAX_TOTAL_PAD_BYTES then
wada.sys.toast(string.format("adding %d bytes would push the pad past the %d byte cap (currently %d)", #text, MAX_TOTAL_PAD_BYTES, emmb(rec)))
return false
end
local ok, err = pcall(function()
rec.bytes = rec.bytes .. text
rec._hex_dirty = true
save_pad(id, rec)
end)
if not ok then
wada.sys.toast("could not add text right now (" .. clean_err(err) .. ")")
return false
end
wada.sys.toast("added " .. #text .. " bytes")
return true
end
function open_pad_text_input()
local id, kind = current_target_id(), dkind
if not id then wada.sys.toast("set a target first"); return end
text_input("Add pad text (typed as raw bytes)", "", function(text)
if text == nil then goto_screen("pad"); return end
local ok = commit_pad_text_bytes(id, kind, text)
goto_screen(ok and "chat" or "pad")
end, "Add")
end
function open_import_hex_input()
local id, kind = current_target_id(), dkind
if not id then wada.sys.toast("set a target first"); return end
text_input("Paste pad hex", "", function(text)
if text == nil then goto_screen("pad"); return end
text = text:gsub("%s+", "")
if not text:match("^[0-9a-fA-F]*$") or #text % 2 ~= 0 then
wada.sys.toast("that doesn't look like valid hex")
goto_screen("pad")
return
end

local rec = current_pad()
local ok, err = import_pad_hex(id, kind, text, rec.role)
if not ok then
wada.sys.toast(err)
goto_screen("pad")
else
wada.sys.toast(string.format("imported %d bytes", math.floor(#text / 2)))
goto_screen("chat")
end
end, "Import")
end
local PANIC_CONFIRM_WINDOW_MS = 4000
local parmed = nil
function do_wipe_data()
wipe_all_pads()
chat_logs = {}
chat_logs_touched_ms = {}
chat_logs_total_entries = 0
sync_status_by_target = {}
pending_resync = {}
resync_badge = {}
sync_backoff = {}
awaiting_reply_since_ms = {}
next_sync_check_ms = {}
last_sync_check_sent_ms = {}
channel_participants = {}
sender_colors = {}
sender_colors_order = {}
imt_by_target = {}
end
function lock_screen_panic_wipe()
local now = wada.sys.millis()
if not parmed or (now - parmed) > PANIC_CONFIRM_WINDOW_MS then
parmed = now
wada.sys.toast("PANIC again to confirm")
return
end
parmed = nil
do_wipe_data()
if wada.store.set then
wada.store.set(PROT_SALT_KEY, "")
wada.store.set(PROT_CHECK_KEY, "")
end
session_key = nil
wada.sys.toast("PANIC WIPE complete, protection off")
goto_screen("home")
end
function finish_enable_protection(passphrase)
local salt = gen_salt_hex()
local key, err = derive_key(passphrase, salt)
if not key then
wada.sys.toast("could not enable protection: " .. tostring(err))
return
end
local tag = check_tag(key)
if not tag then
wada.sys.toast("could not enable protection: check-tag failed")
return
end
if wada.store.set then
wada.store.set(PROT_SALT_KEY, salt)
wada.store.set(PROT_CHECK_KEY, tag)
end
session_key = key
pcall(function()

for id in pairs(known_pad_ids) do
local kind = pad_kind_from_id(id)
if kind then
local rec = load_pad(id, kind)
save_pad(id, rec)
end
end
sset("otp_snd", snd_on and "1" or "0")
sset("otp_dk", dkind)
if dkind == "dm" and type(dtarget) == "table" then
sset("otp_dm", dtarget.name)
elseif dkind == "channel" then
sset("otp_ch", dtarget)
end
end)
wada.sys.toast("passphrase protection enabled")
goto_screen("settings")
end
function open_set_passphrase_input()
if protection_enabled() then
wada.sys.toast("protection is already enabled")
goto_screen("settings")
return
end
text_input("New Passphrase", "", function(first)
if first == nil then goto_screen("settings"); return end
if first == "" then
wada.sys.toast("passphrase cannot be empty")
goto_screen("settings")
return
end
text_input("Confirm Passphrase", "", function(second)
if second == nil then goto_screen("settings"); return end
if second ~= first then
wada.sys.toast("passphrases did not match - try again")
goto_screen("settings")
return
end
finish_enable_protection(first)
end, "Confirm")
end, "Next")
end
function try_unlock(passphrase)
if not passphrase or passphrase == "" then
wada.sys.toast("enter your passphrase")
goto_screen("lock")
return
end
local salt = wada.store.get and wada.store.get(PROT_SALT_KEY)
if type(salt) ~= "string" then salt = "" end
local stored_check = wada.store.get and wada.store.get(PROT_CHECK_KEY)
if type(stored_check) ~= "string" then stored_check = "" end
local key, err = derive_key(passphrase, salt)
if not key then
wada.sys.toast("unlock failed: " .. tostring(err))
goto_screen("lock")
return
end
if check_tag(key) ~= stored_check then
wada.sys.toast("wrong passphrase")
goto_screen("lock")
return
end
session_key = key
load_encrypted_settings()
goto_screen("home")
end
function open_unlock_input()
text_input("Passphrase", "", function(text)
if text == nil then goto_screen("lock"); return end
try_unlock(text)
end, "Unlock")
end
register_screen("lock", function(w, h)
ui_label("OTP Messenger 3.0", 2, 2, 16, PAL.channel)
ui_label("LOCKED - passphrase required", 2, 24, 12, wada.ui.colors.sub)
ui_button("Enter Passphrase", 2, 46, w - 4, 24, safe(open_unlock_input))
ui_button("PANIC WIPE", 2, 74, w - 4, 24, safe(lock_screen_panic_wipe))
end)
local splash_next_screen = "home"
local SPLASH_MS = 900
local splash_started_at = nil
function splash_tick()
if current_screen ~= "splash" or not splash_started_at then return end
if wada.sys.millis() - splash_started_at >= SPLASH_MS then
goto_screen(splash_next_screen)
end
end
register_screen("splash", function(w, h)
ui_label("OTP MESSENGER 3.0", 2, math.floor(h / 2) - 10, 18, PAL.channel)
splash_started_at = wada.sys.millis()
end)
register_screen("home", function(w, h)
ui_label("OTP Messenger 3.0", 2, 2, 16, PAL.channel)
ui_label(BUILD_ID, 2, 20, 9, PAL.ready)
local y = 36
local row_h = 22
local dests = {
{ "Chat", "chat" },
{ "Contacts", "contacts" },
{ "Channels", "channels" },
{ "Pad", "pad" },
{ "Settings", "settings" },
}
local kstate = keynav_start("home")
for _, d in ipairs(dests) do
local label, target = d[1], d[2]
keynav_button(kstate, label, 2, y, w - 4, row_h, safe(function() goto_screen(target) end))
y = y + row_h + 3
end
keynav_finish(kstate)
nav_arrows(2, y, w - 4, 22)
end)
register_screen("chat", function(w, h)
local function measured_line_h(size, fallback)
local ok, mh = pcall(wada.ui.text_h, size)
if ok and type(mh) == "number" and mh > 0 then return math.ceil(mh) end
return fallback
end
local cursor = 1
local title_text = dkind == "dm" and "DM: " .. current_target_label() or "#" .. (dtarget ~= "" and normalize_channel(dtarget) or "?")
local title_row_h = math.max(measured_line_h(14, 18), 20) + 4
local show_strip_toggle = (dkind == "channel")
local strip_toggle_label = show_participant_strip and "[x]People" or "[ ]People"
local ok_ptw, strip_toggle_tw = pcall(wada.ui.text_w, strip_toggle_label, 12)
local strip_toggle_w = (ok_ptw and type(strip_toggle_tw) == "number" and strip_toggle_tw + 12) or 78
local resync_w = 56
local title_reserve = show_strip_toggle and (resync_w + strip_toggle_w + 12) or (resync_w + 6)
local title_lbl = ui_label(title_text, 2, cursor + 2, 14, PAL.channel)
if title_lbl.width then title_lbl:width(w - title_reserve) end
ui_button("ReSync", w - resync_w - 2, cursor, resync_w, title_row_h - 2, safe(resync_current_target))
if show_strip_toggle then
ui_button(strip_toggle_label, w - resync_w - 4 - strip_toggle_w, cursor, strip_toggle_w, title_row_h - 2, safe(toggle_participant_strip))
end
cursor = cursor + title_row_h
local bytes_row_h = measured_line_h(10, 14) + 4
W.chat_bytes_w, W.chat_bytes_h = w - 4, bytes_row_h
local ok_bcv, bcv = pcall(wada.ui.canvas, W.chat_bytes_w, W.chat_bytes_h)
if ok_bcv and bcv then
W.chat_bytes_cv = bcv
pcall(function() bcv:pos(2, cursor) end)
refresh_chat_bytes()
end
cursor = cursor + bytes_row_h
if dkind == "channel" and show_participant_strip then
local pid = current_target_id()
do
local participant_row_h = measured_line_h(9, 12) + 4
W.participant_w, W.participant_h = w - 4, participant_row_h
local ok_pcv, pcv = pcall(wada.ui.canvas, W.participant_w, W.participant_h)
if ok_pcv and pcv then
W.participant_cv = pcv
pcall(function() pcv:pos(2, cursor) end)
refresh_participant_strip()
cursor = cursor + participant_row_h
end
end
else
W.participant_cv = nil
end
local top = cursor + 2
if psend then
W.send_progress_w, W.send_progress_h = w - 4, 6
local ok_cv, cv = pcall(wada.ui.canvas, W.send_progress_w, W.send_progress_h)
if ok_cv and cv then
W.send_progress_cv = cv
pcall(function() cv:pos(2, top) end)
refresh_send_progress()
top = top + 8
end
end
local id = current_target_id()
if id and imt_by_target[id] and not imt_by_target[id].completed then
W.recv_progress_w, W.recv_progress_h = w - 4, 14
local ok_cv2, cv2 = pcall(wada.ui.canvas, W.recv_progress_w, W.recv_progress_h)
if ok_cv2 and cv2 then
W.recv_progress_cv = cv2
pcall(function() cv2:pos(2, top) end)
refresh_recv_progress()
top = top + 16
end
end
local preview_row_h = measured_line_h(12, 16) + 6
local counter_row_h = measured_line_h(10, 14) + 6
local compose_h = chat_compose_active and (preview_row_h + counter_row_h) or 0
local btn_h = 22
local divider_h = 2
W.chat_w, W.chat_h = w - 4, h - top - compose_h - btn_h - divider_h - 6
local ok_cv3, cv3 = pcall(wada.ui.canvas, W.chat_w, W.chat_h)
if ok_cv3 and cv3 then
W.chat_cv = cv3
pcall(function() cv3:pos(2, top) end)
refresh_chat_view()
end
local content_bottom = top + W.chat_h
local ok_dcv, dcv = pcall(wada.ui.canvas, w - 4, divider_h)
if ok_dcv and dcv then
pcall(function() dcv:pos(2, content_bottom + 2) end)
pcall(function() dcv:fill(PAL.channel) end)
end
content_bottom = content_bottom + divider_h + 4
if chat_compose_active then
local avail_w = w - 4
W.compose_preview_w = avail_w
W.compose_preview = ui_label(compose_visible_text(avail_w), 2, content_bottom, 12, wada.ui.colors.text)
if W.compose_preview.width then W.compose_preview:width(avail_w) end
content_bottom = content_bottom + preview_row_h
local counter_w = w - 4 - 50
W.compose_counter = ui_label(compose_counter_text(), 2, content_bottom, 10, PAL.padinfo)
if W.compose_counter.width then W.compose_counter:width(counter_w) end
if W.compose_counter.color then
local rec = current_pad()
local avail = rec and emmb(rec) or 0
W.compose_counter:color(#compose_buffer > avail and PAL.encrypt or PAL.padinfo)
end
ui_button("Send", w - 48, content_bottom - 2, 46, 20, safe(compose_send))
content_bottom = content_bottom + counter_row_h
else
ui_label("Type a message...", 2, content_bottom + 3, 12, PAL.padinfo)
W.compose_hit_y0, W.compose_hit_y1 = content_bottom, content_bottom + 22
content_bottom = content_bottom + 22
end
nav_arrows(2, content_bottom, w - 4, 20, "home")
end)
local contacts_pick_mode = "dm"
register_screen("contacts", function(w, h)
ui_label("Contacts", 2, 2, 16, PAL.channel)
local tlist = make_tracked_list("contacts", 2, 22, w - 4, h - 50)
local contacts = p_cts()
if #contacts == 0 then
if p_cts_last_err then
tlist.add("error: " .. p_cts_last_err)
else
local cc_ok, cc_total = pcall(wada.mesh.contact_count)
tlist.add(string.format("(no contacts found) [count_ok=%s count=%s]", tostring(cc_ok), tostring(cc_total)))
end
else
for _, c in ipairs(contacts) do

pcall(function()
local id = target_id("dm", contact_key(c))

local hint = pad_byte_count_hint(id, "dm")
local suffix = (hint and hint > 0) and string.format(" (%d bytes)", hint) or " (no pad)"
local idx = tlist.add(c.name .. suffix, safe(function()
dtarget = c
dkind = "dm"
pin_dm_target_id()
sset("otp_dm", c.name)
sset("otp_dk", dkind)
goto_screen("chat")
end))
local ok_col, col = pcall(color_for_sender, c.name)
if ok_col then tlist.color(idx, col) end
end)
end
end
tlist.finish()
nav_arrows(2, h - 26, w - 4, 22, "home")
end)
register_screen("channels", function(w, h)
ui_label("Channels", 2, 2, 16, PAL.channel)
local tlist = make_tracked_list("channels", 2, 24, w - 4, h - 52)
local ok, names = pcall(wada.mesh.channels)
if ok and type(names) == "table" and #names > 0 then
for _, nm in ipairs(names) do
local id = target_id("channel", nm)
local hint = pad_byte_count_hint(id, "channel")
local suffix = (hint and hint > 0) and string.format(" (%d bytes)", hint) or " (no pad)"
tlist.add(nm .. suffix, safe(function()
dtarget = nm
dkind = "channel"
sset("otp_ch", dtarget)
sset("otp_dk", dkind)
goto_screen("chat")
end))
end
else
tlist.add("(no channels found - use Set Channel in Settings)")
end
tlist.finish()
nav_arrows(2, h - 26, w - 4, 22, "settings")
end)
local pad_manager_return_to = "settings"
register_screen("pad_manager", function(w, h)
ui_label("Pad Manager", 2, 2, 16, PAL.channel)
local tlist = make_tracked_list("pad_manager", 2, 24, w - 4, h - 52)
local function suffix_for(count)
if count == nil then return " (?)" end
if count > 0 then return string.format(" (%d bytes)", count) end
return " (no pad)"
end
local any = false
for _, c in ipairs(p_cts()) do
any = true
local id = target_id("dm", contact_key(c))
local idx = tlist.add(c.name .. suffix_for(pad_byte_count_hint(id, "dm")), safe(function()
dtarget = c
dkind = "dm"
pin_dm_target_id()
sset("otp_dm", c.name)
sset("otp_dk", dkind)
goto_screen("pad")
end))
local ok_col, col = pcall(color_for_sender, c.name)
if ok_col then tlist.color(idx, col) end
end
local ok_ch, names = pcall(wada.mesh.channels)
if ok_ch and type(names) == "table" then
for _, nm in ipairs(names) do
any = true
local id = target_id("channel", nm)
local label = nm .. (is_public_channel_name(nm) and " (blocked)" or suffix_for(pad_byte_count_hint(id, "channel")))
tlist.add(label, safe(function()
dtarget = nm
dkind = "channel"
sset("otp_ch", dtarget)
sset("otp_dk", dkind)
goto_screen("pad")
end))
end
end
if not any then
tlist.add("(no contacts or channels found)")
end
tlist.finish()
nav_arrows(2, h - 26, w - 4, 22, pad_manager_return_to)
end)
function open_channel_input()
text_input("Channel name", (dkind == "channel" and dtarget) or "", function(text)
if text == nil then goto_screen("settings"); return end
dtarget = text
dkind = "channel"
sset("otp_ch", dtarget)
sset("otp_dk", dkind)
goto_screen("settings")
end, "Set")
end
register_screen("settings", function(w, h)
ui_label("Settings", 2, 2, 16, PAL.channel)
local y = 18
local kstate = keynav_start("settings")
local snd_lbl = ui_label(snd_on and "Sound: on" or "Sound: off", 2, y, 12, PAL.plaintext)
keynav_button(kstate, "Toggle", w - 60, y - 3, 56, 18, safe(function()
snd_on = not snd_on
snd_lbl:set(snd_on and "Sound: on" or "Sound: off")
sset("otp_snd", snd_on and "1" or "0")
end))
y = y + 19
local thm_lbl = ui_label("Theme: " .. THNAMES[thmidx], 2, y, 12, PAL.padinfo)
keynav_button(kstate, "Cycle", w - 60, y - 3, 56, 18, safe(function()
thmidx = (thmidx % #THNAMES) + 1
apply_theme(thmidx)
sset_plain("otp_thm", thmidx)
goto_screen("settings")
end))
y = y + 19
if protection_enabled() then
ui_label("Protection: Enabled", 2, y, 12, PAL.padinfo)
y = y + 19
else
keynav_button(kstate, "Set Passphrase...", 2, y, w - 4, 20, safe(open_set_passphrase_input))
y = y + 21
end
local half_w = math.floor((w - 6) / 2)
keynav_button(kstate, "Set Channel...", 2, y, half_w, 20, safe(open_channel_input))
keynav_button(kstate, "Browse...", 6 + half_w, y, half_w, 20, safe(function() goto_screen("channels") end))
y = y + 21
keynav_button(kstate, "Set DM Contact...", 2, y, w - 4, 20, safe(function() goto_screen("contacts") end))
y = y + 21
keynav_button(kstate, "Pad Manager...", 2, y, w - 4, 20, safe(function() pad_manager_return_to = "settings"; goto_screen("pad_manager") end))
y = y + 21
keynav_finish(kstate)
ui_label(dkind == "dm" and ("Dest: DM (" .. current_target_label() .. ")") or ("Dest: Channel (" .. current_target_label() .. ")"), 2, y, 12, PAL.channel)
y = y + 14
ui_label("Build: " .. BUILD_ID, 2, y, 9, PAL.padinfo)
y = y + 12
ui_label(string.format("Lua free: ~%.0f KB", mt_lua() / 10), 2, y, 9, PAL.padinfo)
y = y + 12
nav_arrows(2, y, w - 4, 19, "home")
end)
register_screen("themes", function(w, h)
ui_label("Themes", 2, 2, 16, PAL.channel)
local list_h = math.max(20, h - 24 - 26)
local tlist = make_tracked_list("themes", 2, 24, w - 4, list_h)
for i = 1, #THNAMES do
local is_active = (i == thmidx)
local label = THNAMES[i] .. (is_active and " (active)" or "")
tlist.add(label, safe(function()
thmidx = i
apply_theme(thmidx)
sset_plain("otp_thm", thmidx)
goto_screen("themes")
end))
end
list_state["themes"].selected_idx = thmidx
tlist.finish()
nav_arrows(2, h - 26, w - 4, 22, "settings")
end)
local wipe_pad_armed_at = nil
local wipe_pad_armed_id = nil
function wipe_pad_tap(id, kind)
local now = wada.sys.millis()
if wipe_pad_armed_id ~= id or not wipe_pad_armed_at or (now - wipe_pad_armed_at) > PANIC_CONFIRM_WINDOW_MS then
wipe_pad_armed_at = now
wipe_pad_armed_id = id
wada.sys.toast("tap Wipe again to confirm")
return false
end
wipe_pad_armed_at = nil
wipe_pad_armed_id = nil
wipe_pad(id, kind)
return true
end
local pad_import_target_id, pad_import_target_kind, pad_import_target_role = nil, nil, nil
register_screen("pad_import", function(w, h)
ui_label("Import Pad File", 2, 2, 16, PAL.channel)
ui_label("into: " .. current_target_label(), 2, 20, 11, PAL.padinfo)
local tlist = make_tracked_list("pad_import", 2, 36, w - 4, h - 64)
local importable = scan_importable_files()
if #importable == 0 then
tlist.add("(no unclaimed .hex/.txt files found)")
else
for _, entry in ipairs(importable) do
tlist.add(entry.filename .. " (" .. entry.kind .. ")", safe(function()
local ok, err = import_pad_file(entry, pad_import_target_id, pad_import_target_kind, pad_import_target_role)
if not ok then
wada.sys.toast(err)
else
wada.sys.toast("imported " .. entry.filename)
goto_screen("chat")
end
end))
end
end
tlist.finish()
nav_arrows(2, h - 26, w - 4, 22, "pad")
end)
register_screen("pad", function(w, h)
if dkind == "channel" and is_public_channel_name(dtarget) then
ui_label("Pad: " .. current_target_label(), 2, 2, 14, PAL.channel)
local y = 30
local msg_w = w - 4
local lbl = ui_label(
"OTP isn't available on the Public channel. It's shared by everyone "
.. "on the mesh using the same well-known key, so a pad only you and "
.. "the group's members hold can't actually keep it private - use a "
.. "private channel or a DM if you want OTP encryption.",
2, y, 11, PAL.padinfo
)
if lbl.width then lbl:width(msg_w) end
nav_arrows(2, h - 24, w - 4, 20, "home")
return
end
ui_label("Pad: " .. current_target_label(), 2, 2, 14, PAL.channel)
W.pad_info_w, W.pad_info_h = w - 4, 48
local ok_picv, picv = pcall(wada.ui.canvas, W.pad_info_w, W.pad_info_h)
if ok_picv and picv then
W.pad_info_cv = picv
pcall(function() W.pad_info_cv:pos(2, 16) end)
refresh_pad_info()
end
local id, kind = current_target_id(), dkind
local y = 66
local kstate = keynav_start("pad")
if not id then
ui_label("Pick a contact or channel first (Home > Contacts/Channels)", 2, y, 11, PAL.padinfo)
keynav_finish(kstate)
nav_arrows(2, h - 24, w - 4, 20, "home")
return
end
local rec = load_pad(id, kind)
local function bw(label)
local ok, tw = pcall(wada.ui.text_w, label, 12)
return ((ok and type(tw) == "number") and math.floor(tw) or (#label * 7)) + 14
end
local prot_label = keep_pad_files and "Protect: ON" or "Protect: OFF"
local flip_w = (kind == "dm") and bw("Flip") or 0
local prot_w = bw("Protect: OFF")
local flip_x = w - 2 - flip_w
local prot_x = flip_x - (flip_w > 0 and 2 or 0) - prot_w
if kind == "dm" then
local auto_role = resolve_dm_role(dtarget)
local role_lbl = ui_label("Role: " .. rec.role .. (auto_role == rec.role and " (auto)" or " (manual)"), 2, y, 10, PAL.padinfo)
if role_lbl.width then role_lbl:width(math.max(20, prot_x - 4)) end
end
keynav_button(kstate, prot_label, prot_x, y - 3, prot_w, 16, safe(toggle_keep_pad_files))
if kind == "dm" then
keynav_button(kstate, "Flip", flip_x, y - 3, flip_w, 16, safe(function()
rec.role = (rec.role == "A") and "B" or "A"
save_pad(id, rec)
goto_screen("pad")
end))
end
y = y + 20
local half = math.floor((w - 6) / 2)
local importable = scan_importable_files()
keynav_button(kstate, importable[1] and ("Import File (" .. #importable .. ")") or "Import File", 2, y, half, 20, safe(function()
if #importable == 0 then
wada.sys.toast("no unclaimed .hex/.txt files found")
return
end
pad_import_target_id, pad_import_target_kind, pad_import_target_role = id, kind, rec.role
goto_screen("pad_import")
end))
keynav_button(kstate, "Generate 1000 bytes", 6 + half, y, half, 20, safe(function() start_pad_generation(id, kind, 1000) end))
y = y + 21
keynav_button(kstate, "Add Text...", 2, y, half, 20, safe(open_pad_text_input))
keynav_button(kstate, "Add Hex...", 6 + half, y, half, 20, safe(open_import_hex_input))
y = y + 21
keynav_button(kstate, "Wipe This Pad...", 2, y, half, 20, safe(function()
if wipe_pad_tap(id, kind) then goto_screen("pad") end
end))
keynav_button(kstate, "Pad Manager...", 6 + half, y, half, 20, safe(function() pad_manager_return_to = "pad"; goto_screen("pad_manager") end))
y = y + 21
keynav_button(kstate, "Check Sync", 2, y, w - 4, 20, safe(function()
request_prompt_sync_check(id)
sync_status_by_target[id] = { ok = nil, text = "checking..." }
refresh_pad_sync_label()
send_sync_check(id, kind, rec, false)
end))
y = y + 21
keynav_finish(kstate)
nav_arrows(2, y, w - 4, 19, "home")
end)
function load_unencrypted_settings()
local saved_thm = tonumber(sget_plain("otp_thm", "1")) or 1
local thm_map = { 1, 2, 3, 4, 1, 5, 1, 6, 1 }
saved_thm = thm_map[saved_thm] or 1
if saved_thm >= 1 and saved_thm <= #THNAMES then thmidx = saved_thm end
apply_theme(thmidx)
keep_pad_files = sget_plain("otp_keeppad", "1") ~= "0"
show_participant_strip = sget_plain("otp_showsync", "0") ~= "0"
end
function toggle_keep_pad_files()
keep_pad_files = not keep_pad_files
sset_plain("otp_keeppad", keep_pad_files and "1" or "0")
wada.sys.toast(keep_pad_files and "pad files will be kept after import" or "pad files will be deleted after import")
if current_screen == "pad" then goto_screen("pad") end
end
function toggle_participant_strip()
show_participant_strip = not show_participant_strip
sset_plain("otp_showsync", show_participant_strip and "1" or "0")
wada.sys.toast(show_participant_strip and "participant sync strip: on" or "participant sync strip: off")
if current_screen == "chat" then goto_screen("chat") end
end
function load_encrypted_settings()
snd_on = sget("otp_snd", "1") ~= "0"
local saved_ch = sget("otp_ch", "")
local saved_dm = sget("otp_dm", "")
local saved_dk = sget("otp_dk", "channel")
if saved_dk == "dm" and saved_dm ~= "" then
local c = find_contact_by_name(saved_dm)
dtarget = c or { name = saved_dm, pubkey = nil }
dkind = "dm"
pin_dm_target_id()
elseif saved_ch ~= "" then
dtarget = saved_ch
dkind = "channel"
else
dtarget, dkind = "", "channel"
end
end
function do_on_open(w, h)
screen_w, screen_h = w, h
load_unencrypted_settings()
load_encrypted_settings()
splash_next_screen = protection_enabled() and "lock" or "home"
goto_screen("splash")
wada.sys.toast("otp messenger 3.0: ready")
if wada.timer and wada.timer.every then
pcall(wada.timer.every, 200)
end
end
function app.on_open(w, h)
local ok, err = pcall(do_on_open, w, h)
if not ok then
wada.sys.toast("crashed: " .. tostring(err))
end
end
function app.on_tick()
pcall(screen_rebuild_tick)
pcall(splash_tick)

if current_screen == "chat" and (psend or W.send_progress_cv) then
pcall(refresh_send_progress)
end
if current_screen == "chat" then
local id = current_target_id()
if id and imt_by_target[id] then pcall(refresh_recv_progress) end
end
local ok2, err2 = pcall(pad_gen_tick)
if not ok2 then
wada.sys.toast("pad generation error: " .. tostring(err2))
end
local ok5, err5 = pcall(psend_tick)
if not ok5 then
wada.sys.toast("send retry error: " .. tostring(err5))
end
local ok5c, err5c = pcall(ssend_tick)
if not ok5c then
wada.sys.toast("send retry error: " .. tostring(err5c))
end
local ok5b, err5b = pcall(pad_save_tick)
if not ok5b then
wada.sys.toast("pad save retry error: " .. tostring(err5b))
end
local ok5e, err5e = pcall(encrypt_retry_tick)
if not ok5e then
wada.sys.toast("encrypt retry error: " .. tostring(err5e))
end
local ok5f, err5f = pcall(reassembly_retry_tick)
if not ok5f then
wada.sys.toast("reassembly retry error: " .. tostring(err5f))
end
local ok5d, err5d = pcall(channel_jitter_tick)
if not ok5d then
wada.sys.toast("channel send error: " .. tostring(err5d))
end
pcall(flush_queued_send)
pcall(auto_sync_tick)
pcall(nack_tick)
pcall(repeat_tick)
pcall(sync_backoff_tick)
pcall(maybe_auto_sync_check)
pcall(resync_badge_tick)
pcall(waiting_tick)
pcall(signal_tick)
pcall(channel_participants_prune_tick)
end
function app.on_input(ev)
local ok, err = pcall(function()
if current_screen == "splash" then
goto_screen(splash_next_screen)
return
end
if chat_compose_active then
chat_compose_handle_input(ev)
return
end
if current_screen == "chat" and ev.type == "up" and W.compose_hit_y0 and ev.y and ev.y >= W.compose_hit_y0 and ev.y <= W.compose_hit_y1 then
open_compose_input()
return
end
if current_screen == "chat" and ev.type == "dir" and (ev.dir == "up" or ev.dir == "down") then
scroll_chat(ev.dir == "up" and -1 or 1)
return
end
if current_screen == "chat" and (ev.type == "dir" or ev.type == "swipe") and (ev.dir == "left" or ev.dir == "right") then
return
end
if current_screen == "chat" and ev.type == "swipe" and (ev.dir == "up" or ev.dir == "down") then
scroll_chat(ev.dir == "up" and 1 or -1)
return
end
if list_state[current_screen] then
local state = list_state[current_screen]
if (ev.type == "dir" or ev.type == "swipe") and (ev.dir == "up" or ev.dir == "down") then
if state.count > 0 then
state.selected_idx = state.selected_idx + (ev.dir == "up" and -1 or 1)
if state.selected_idx < 1 then state.selected_idx = 1 end
if state.selected_idx > state.count then state.selected_idx = state.count end
pcall(function() state.widget:select(state.selected_idx) end)
end
return
elseif ((ev.type == "dir" or ev.type == "swipe") and ev.dir == "select")
or (ev.type == "key" and ev.key == "enter")
or (ev.type == "up" and math.abs((ev.x or -9999) - math.floor(screen_w / 2)) <= 2
and math.abs((ev.y or -9999) - math.floor(screen_h / 2)) <= 2) then
if state.count > 0 and state.callbacks[state.selected_idx] then
state.callbacks[state.selected_idx]()
end
return
end
end
if not kb_active and current_screen ~= "pad" and (ev.type == "dir" or ev.type == "swipe") and (ev.dir == "left" or ev.dir == "right") then
if ev.dir == "left" then page_prev() else page_next() end
return
end

local send_in_flight = psend or ssend or pending_channel_send or queued_cipher or pending_encrypt_retry
if not kb_active and not send_in_flight and ev.type == "key" and ev.code == 131 then
goto_screen("contacts")
return
end
if not kb_active and not send_in_flight and ev.type == "key" and ev.code == 132 then
goto_screen("pad")
return
end
if not kb_active and current_screen == "chat" and ev.type == "key"
and type(ev.key) == "string" and #ev.key == 1 then
open_compose_input()
chat_compose_handle_input(ev)
return
end
if not kb_active and ev.type == "key" and ev.key == "backspace" and current_back_target then
goto_screen(current_back_target)
return
end
kb_handle_input(ev)
end)
if not ok then
wada.sys.toast("input error: " .. tostring(err))
end
pcall(psend_tick)
pcall(pad_save_tick)
pcall(flush_queued_send)
end
return app
