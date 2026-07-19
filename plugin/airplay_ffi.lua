--[[
    airplay_ffi.lua — FFI bindings to libairplay_mirror.so
    Loads the compiled C library and exposes a simple Lua API.
--]]

local ffi = require("ffi")

local _src = debug.getinfo(1, "S").source
local PLUGIN_DIR = _src:match("^@(.+/)") or "./"
local LOG_PATH = PLUGIN_DIR .. "airplay.log"
local function log(msg)
    local f = io.open(LOG_PATH, "a")
    if f then
        f:write(os.date("%Y-%m-%d %H:%M:%S") .. " " .. tostring(msg) .. "\n")
        f:close()
    end
end
log("airplay_ffi: module loading started")

ffi.cdef[[
    typedef void (*airplay_frame_cb)(const uint8_t *gray8, int width, int height, void *userdata);

    typedef struct {
        const char      *device_name;
        int              http_port;
        int              video_port;
        airplay_frame_cb on_frame;
        void            *userdata;
    } airplay_config_t;

    int  airplay_mirror_start(const airplay_config_t *cfg);
    void airplay_mirror_stop(void);
    int  airplay_mirror_get_frame(uint8_t *out_gray8, int *out_w, int *out_h);
    int  airplay_mirror_render_to_fb(uint8_t *fb, int fb_w, int fb_h, int fb_stride);
    int  airplay_mirror_render_direct(void);
    int  airplay_mirror_clear_direct(void);
    int  airplay_mdns_start(const char *device_name, int port);
    void airplay_mdns_stop(void);
]]

-- Build candidate paths for libairplay_mirror.so
local function find_lib()
    local src = debug.getinfo(1, "S").source  -- e.g. "@plugins/airplay.koplugin/airplay_ffi.lua"
    local script_dir = src:match("^@(.+/)") or "./"

    -- Also try to get absolute path via lfs if available
    local abs_prefix = ""
    local ok_lfs, lfs = pcall(require, "lfs")
    if ok_lfs then abs_prefix = (lfs.currentdir() or "") .. "/" end

    local candidates = {
        script_dir .. "libairplay_mirror.so",
        abs_prefix .. script_dir .. "libairplay_mirror.so",
        "/mnt/us/koreader/plugins/airplay.koplugin/libairplay_mirror.so",
        "/mnt/us/airplay/libairplay_mirror.so",
    }
    local tried = {}
    for _, path in ipairs(candidates) do
        table.insert(tried, path)
        local f = io.open(path, "rb")
        if f then
            f:close()
            log("find_lib: found at " .. path)
            return path
        else
            log("find_lib: not found: " .. path)
        end
    end
    return nil, table.concat(tried, "\n  ")
end

log("find_lib: src=" .. tostring(debug.getinfo(1,"S").source))
local lib_path, tried_paths = find_lib()
if not lib_path then
    local msg = "libairplay_mirror.so not found.\nTried:\n  " .. (tried_paths or "?")
    log("ERROR: " .. msg)
    error(msg)
end

log("ffi.load: attempting " .. lib_path)
local load_ok, load_err = pcall(ffi.load, lib_path)
if not load_ok then
    local msg = "ffi.load('" .. lib_path .. "') failed: " .. tostring(load_err)
    log("ERROR: " .. msg)
    error(msg)
end
log("ffi.load: success")
local lib = ffi.load(lib_path)

-- Frame buffer: max 1920×1200 grayscale
local MAX_W, MAX_H = 1920, 1200
local frame_buf = ffi.new("uint8_t[?]", MAX_W * MAX_H)
local frame_w   = ffi.new("int[1]")
local frame_h   = ffi.new("int[1]")
-- Screen-sized output buffer for render_to_fb (max Kindle screen ~1440×1920)
local MAX_SW, MAX_SH = 1440, 1920
local screen_fb = ffi.new("uint8_t[?]", MAX_SW * MAX_SH)

local M = {}

log("airplay_ffi: module ready")

function M.start(opts)
    opts = opts or {}
    log("M.start: device=" .. (opts.device_name or "Kindle AirPlay"))
    local cfg = ffi.new("airplay_config_t")
    cfg.device_name = opts.device_name or "Kindle AirPlay"
    cfg.http_port   = opts.http_port   or 7000
    cfg.video_port  = opts.video_port  or 7100
    cfg.on_frame    = nil
    cfg.userdata    = nil
    local ok = lib.airplay_mirror_start(cfg) == 0
    log("M.start: result=" .. tostring(ok))
    return ok
end

function M.stop()
    lib.airplay_mirror_stop()
end

--[[
    Returns gray8 string + w, h if new frame available; nil otherwise.
    Called from KOReader UI timer at ~1-2 Hz (e-ink safe rate).
--]]
function M.get_frame()
    if lib.airplay_mirror_get_frame(frame_buf, frame_w, frame_h) ~= 0 then
        return nil
    end
    local w = frame_w[0]
    local h = frame_h[0]
    if w <= 0 or h <= 0 then return nil end
    return ffi.string(frame_buf, w * h), w, h
end

--[[
    Scale current frame into caller-supplied fb pointer (gray8, stride bytes/row).
    Returns 0 if a new frame was written, -1 if no frame ready.
    Called from render_frame with Screen.bb.data pointer to avoid Lua BlitBuffer.
--]]
function M.render_to_fb(fb_ptr, fb_w, fb_h, fb_stride)
    return lib.airplay_mirror_render_to_fb(fb_ptr, fb_w, fb_h, fb_stride)
end

function M.render_direct()
    return lib.airplay_mirror_render_direct()
end

function M.clear_direct()
    return lib.airplay_mirror_clear_direct()
end

return M
