--[[
    main.lua — KOReader plugin: AirPlay Screen Mirror Receiver

    Receives macOS AirPlay mirroring, renders decoded grayscale frames
    to Kindle e-ink display at ~1-2 FPS (e-ink safe).

    Install: copy plugin/ to <koreader>/plugins/airplay.koplugin/
             copy libairplay_mirror.so to same directory
--]]

local WidgetContainer = require("ui/widget/container/widgetcontainer")
local UIManager       = require("ui/uimanager")
local Device          = require("device")
local Screen          = Device.screen
local InfoMessage     = require("ui/widget/infomessage")
local ConfirmBox      = require("ui/widget/confirmbox")
local _               = require("gettext")

local AirPlayPlugin = WidgetContainer:extend{
    name        = "airplay",
    is_doc_only = false,
}

function AirPlayPlugin:init()
    if self.ui and self.ui.menu then
        self.ui.menu:registerToMainMenu(self)
    end
end

-- Refresh interval for e-ink (ms): 2000ms = 0.5 FPS
local POLL_INTERVAL_MS = 2000
-- GC16 waveform handles ghosting; keep counter for potential future use
local FULL_REFRESH_EVERY = 6

local airplay_ffi   -- loaded lazily
local running       = false
local partial_count = 0
local current_bb    = nil  -- current BlitBuffer shown on screen

-- ─── helpers ──────────────────────────────────────────────────────

local function show_message(msg)
    UIManager:show(InfoMessage:new{ text = msg, timeout = 3 })
end

local function get_network_ip()
    local f = io.popen("ip route get 1 2>/dev/null | awk '{print $NF; exit}'")
    if not f then return "unknown" end
    local ip = f:read("*l") or "unknown"
    f:close()
    return ip
end

local function open_firewall()
    os.execute("iptables -I INPUT  -p tcp --dport 7000 -j ACCEPT 2>/dev/null")
    os.execute("iptables -I INPUT  -p tcp --dport 7100 -j ACCEPT 2>/dev/null")
    os.execute("iptables -I INPUT  -p udp --dport 5353 -j ACCEPT 2>/dev/null")
    os.execute("iptables -I OUTPUT -p tcp --sport 7000 -j ACCEPT 2>/dev/null")
    os.execute("iptables -I OUTPUT -p tcp --sport 7100 -j ACCEPT 2>/dev/null")
    os.execute("iptables -I OUTPUT -p udp --sport 5353 -j ACCEPT 2>/dev/null")
end

local function close_firewall()
    os.execute("iptables -D INPUT  -p tcp --dport 7000 -j ACCEPT 2>/dev/null")
    os.execute("iptables -D INPUT  -p tcp --dport 7100 -j ACCEPT 2>/dev/null")
    os.execute("iptables -D INPUT  -p udp --dport 5353 -j ACCEPT 2>/dev/null")
    os.execute("iptables -D OUTPUT -p tcp --sport 7000 -j ACCEPT 2>/dev/null")
    os.execute("iptables -D OUTPUT -p tcp --sport 7100 -j ACCEPT 2>/dev/null")
    os.execute("iptables -D OUTPUT -p udp --sport 5353 -j ACCEPT 2>/dev/null")
end

-- ─── frame rendering ──────────────────────────────────────────────

local function render_frame()
    -- Write directly to /dev/fb0 + trigger mxcfb eink ioctl from C.
    -- Bypasses KOReader Screen/BlitBuffer entirely (avoids blitbuffer.lua crashes).
    if airplay_ffi.render_direct() ~= 0 then
        return  -- no new frame or fb unavailable
    end
    partial_count = (partial_count + 1) % FULL_REFRESH_EVERY
end

-- ─── poll loop ────────────────────────────────────────────────────

local function poll()
    if not running then return end

    local ok, err = pcall(render_frame)
    if not ok then
        local _src = debug.getinfo(1, "S").source
        local _dir = _src:match("^@(.+/)") or "./"
        local f = io.open(_dir .. "airplay.log", "a")
        if f then f:write("render_frame error: " .. tostring(err) .. "\n"); f:close() end
    end

    -- Reschedule
    UIManager:scheduleIn(POLL_INTERVAL_MS / 1000.0, poll)
end

-- ─── start / stop ─────────────────────────────────────────────────

local function start_receiver()
    if running then
        show_message(_("AirPlay already running"))
        return
    end

    -- Lazy-load FFI module
    if not airplay_ffi then
        local ok, mod = pcall(require, "airplay_ffi")
        if not ok then
            UIManager:show(InfoMessage:new{
                text = "AirPlay load error:\n" .. tostring(mod),
            })
            return
        end
        airplay_ffi = mod
    end

    open_firewall()

    local ok = airplay_ffi.start({
        device_name = "Kindle AirPlay",
        http_port   = 7000,
        video_port  = 7100,
    })
    if not ok then
        close_firewall()
        show_message(_("Failed to start AirPlay receiver.\nCheck port availability."))
        return
    end

    running = true
    partial_count = 0

    local ip = get_network_ip()
    show_message(string.format(
        _("AirPlay receiver started!\n\nOn your Mac:\nSystem Preferences → Displays → Add Display → AirPlay Display\nSelect: Kindle AirPlay\n\nKindle IP: %s"), ip))

    UIManager:scheduleIn(POLL_INTERVAL_MS / 1000.0, poll)
end

local function stop_receiver()
    if not running then return end
    running = false
    close_firewall()
    UIManager:unschedule(poll)
    if airplay_ffi then airplay_ffi.stop() end
    if current_bb then
        current_bb:free()
        current_bb = nil
    end
    UIManager:setDirty(nil, "full")
    show_message(_("AirPlay stopped"))
end

-- ─── KOReader plugin hooks ────────────────────────────────────────

function AirPlayPlugin:addToMainMenu(menu_items)
    menu_items.airplay_mirror = {
        text         = _("AirPlay Mirror"),
        sorting_hint = "more_tools",
        sub_item_table = {
            {
                text = _("Start AirPlay receiver"),
                callback = function() start_receiver() end,
            },
            {
                text = _("Stop AirPlay receiver"),
                callback = function()
                    UIManager:show(ConfirmBox:new{
                        text = _("Stop AirPlay receiver?"),
                        ok_callback = function() stop_receiver() end,
                    })
                end,
            },
            {
                text = _("Connection info"),
                callback = function()
                    local ip = get_network_ip()
                    local status = running and _("Running") or _("Stopped")
                    UIManager:show(InfoMessage:new{
                        text = string.format(
                            _("Status: %s\nKindle IP: %s\nAirPlay port: 7000\n\nOn Mac: System Prefs → Displays → AirPlay Display → Kindle AirPlay"),
                            status, ip)
                    })
                end,
            },
            {
                text_func = function()
                    return string.format(_("Refresh rate: %d ms"), POLL_INTERVAL_MS)
                end,
                sub_item_table = {
                    {
                        text = _("0.5 FPS (2000ms) — least ghosting"),
                        callback = function()
                            POLL_INTERVAL_MS = 2000
                            FULL_REFRESH_EVERY = 4
                        end,
                    },
                    {
                        text = _("1 FPS (1000ms) — default"),
                        callback = function()
                            POLL_INTERVAL_MS = 1000
                            FULL_REFRESH_EVERY = 6
                        end,
                    },
                },
            },
        },
    }
end

function AirPlayPlugin:onCloseDocument()
    if running then stop_receiver() end
end

function AirPlayPlugin:onExit()
    if running then stop_receiver() end
end

return AirPlayPlugin
