-- hyprland.lua fixture: runs in the importer's sandbox.
local mode = "a"
local file = io.open(os.getenv("HOME") .. "/.config/hypr/mode", "r") -- lands in this copy
if file then
    mode = file:read("*l")
    file:close()
end
assert(io.open("/etc/hostname") == nil, "reads outside the directory must fail")
assert(io.open("mode", "w") == nil, "writes must fail")
assert(os.execute == nil and io.popen == nil and os.remove == nil, "no processes or writes")
assert(loadfile("/etc/hostname") == nil, "loadfile outside the directory must fail")
local theme = require("theme")

if mode == "b" then
    hl.monitor({ output = "DP-1", mode = "3840x2160@60", position = "0x0", scale = "2" })
else
    hl.monitor({ output = "DP-1", mode = "1920x1080@60", position = "0x0", scale = "1" })
end
hl.monitor({ output = "DP-2", disabled = true })
hl.config({
    general = {
        gaps_in = 2,
        col = {
            active_border = { colors = { "rgba(e8a3c9ff)", "rgba(c490ffff)" }, angle = 35 },
            inactive_border = 0xee1a1a1a,
        },
    },
})
hl.config({ decoration = { inactive_opacity = 0.9 }, animations = { enabled = false },
            input = { kb_layout = "us", touchpad = { ["tap-to-click"] = false } } })
hl.window_rule({ name = "kitty", match = { class = "^kitty$" }, opacity = "0.9 0.8" })
local zen = hl.window_rule({ match = { class = "^zen$" }, opacity = 0.5 })
zen:set_enabled(false)
local bind = hl.bind("SUPER + Q", hl.dsp.exec_cmd("kitty"))
bind:set_enabled(true)
hl.on("hyprland.start", function() error("handlers never run") end)
hl.bind("Print", hl.dsp.exec_cmd(hyde.sh.screenshot.snip()))
hl.config(theme)
error("boom")
hl.config({ general = { gaps_out = 99 } })
