-- Minimal wada.* host for running OTP_3_RC1.lua inside test_otp_interop.
-- UI calls are accepted and ignored; mesh sends go to the C side (host_*),
-- which routes them to the native OTP core.
local function widget()
  return setmetatable({}, { __index = function() return function() return 0 end end })
end

STORE, FILES, TOASTS = {}, {}, {}

local CONTACT = { name = PEER_NAME, pubkey = PEER_PK, type = 1 }

wada = {
  ui = {
    colors = { text = 0xFFFFFF, sub = 0xAAAAAA },
    clear = function() end,
    scroll = function() end,
    label = function() return widget() end,
    button = function() return widget() end,
    canvas = function() return widget() end,
    list = function() return widget() end,
    input = function() end,
    text_w = function(s, _) return #s * 6 end,
    text_h = function(_) return 12 end,
    text_lines = function() return 1 end,
  },
  sys = {
    millis = function() return host_millis() end,
    epoch = function() return 0 end,
    random = function(n) return host_random(n) end,
    toast = function(s) TOASTS[#TOASTS + 1] = s; if VERBOSE then print("[lua toast] " .. s) end end,
    beep = function() return true end,
  },
  timer = { every = function() end },
  store = {
    get = function(k, d) local v = STORE[k]; if v == nil then return d end; return v end,
    set = function(k, v) STORE[k] = v; return true end,
  },
  fs = {
    read = function(n) return FILES[n] end,
    write = function(n, d) FILES[n] = d; return true end,
    remove = function(n) FILES[n] = nil; return true end,
    list = function()
      local out = {}
      for k in pairs(FILES) do out[#out + 1] = { name = k } end
      return out
    end,
  },
  mesh = {
    NODE_REPEATER = 2, NODE_ROOM = 3, NODE_SENSOR = 4,
    self = function() return { name = SELF_NAME, pubkey = SELF_PK } end,
    contacts = function() return { CONTACT } end,
    contact_count = function() return 1 end,
    channels = function() return { "Public", CHANNEL } end,
    stats = function() return { rssi = -80, noise = -110, tx_budget_ms = 5000, duty_pct = 10 } end,
    repeats = function() return 0 end,
    send_dm = function(to, text) return host_send_dm(to, text), "sent" end,
    send = function(ch, text) if host_send(ch, text) then return true, 1 end return false, "no such channel" end,
  },
  crypto = {
    hex = function(s) return host_hex(s) end,
    hmac_sha256 = function(k, m) return host_hmac(k, m) end,
  },
}
