-- shaoDe configuration, API version 1.
-- Launch commands are argument arrays, never shell strings.
-- Alt avoids most shortcuts reserved by the host compositor when nested.
local mod = "Alt"

return {
    version = 1,
    appearance = { background = "#19212e" },
    keyboard = { layout = "us", options = "", repeat_rate = 25, repeat_delay = 600 },
    mouse = { modifier = mod }, -- modifier + left drag moves; right drag resizes
    layout = { gap = 8 },
    startup = {}, -- e.g. { { "kitty" } }
    bindings = {
        { mods = { mod }, key = "Return", action = "spawn", command = { "kitty" } },
        { mods = { mod }, key = "Tab", action = "cycle" },
        { mods = { mod }, key = "F4", action = "close" },
        { mods = { mod }, key = "Left", action = "snap_left" },
        { mods = { mod }, key = "Right", action = "snap_right" },
        { mods = { mod }, key = "Up", action = "maximize" },
        { mods = { mod }, key = "Down", action = "restore" },
        { mods = { mod }, key = "t", action = "tile" },
        { mods = { mod, "Shift" }, key = "r", action = "reload" },
        { mods = { mod, "Shift" }, key = "Escape", action = "quit" },
    },
}
