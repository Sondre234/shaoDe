-- shaoDe configuration, API version 1.
-- Launch commands are argument arrays, never shell strings.
-- Alt avoids most shortcuts reserved by the host compositor when nested.
local mod = "Alt"

return {
    version = 1,
    appearance = { background = "#19212e" },
    keyboard = { layout = "us", options = "", repeat_rate = 25, repeat_delay = 600 },
    mouse = { modifier = mod }, -- modifier + left drag moves; right drag resizes
    layout = {
        gap = 8,
        workspaces = 4,
        tiling = false, -- start with automatic tiling; the panel button toggles it
    },
    outputs = {
        -- Left to right by connector name; unlisted monitors follow on the right.
        order = { "HDMI-A-1", "DP-3", "DP-1" },
        primary = "DP-3", -- the cursor starts here; without it, the leftmost monitor
    },
    xwayland = true, -- run X11 applications; Xwayland starts on first use (restart to change)
    shell = {
        enabled = true,
        panel_height = 52,
        accent = "#7da8ff",
        panel_color = "#151e2c",
        text_color = "#edf2fa",
        wallpaper = "", -- absolute path, or relative to this configuration file
        launchers = {
            { name = "Terminal", icon = "utilities-terminal", command = { "kitty" } },
            { name = "Home", icon = "user-home", command = { "xdg-open", "." } },
        },
    },
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
        { mods = { mod, "Shift" }, key = "t", action = "toggle_tiling" },
        { mods = { mod, "Shift" }, key = "f", action = "toggle_floating" },
        { mods = { mod }, key = "F11", action = "fullscreen" },
        -- Lock with any ext-session-lock client, e.g.:
        -- { mods = { "Super" }, key = "l", action = "spawn", command = { "swaylock" } },
        { mods = { "Ctrl", mod }, key = "Right", action = "workspace_next" },
        { mods = { "Ctrl", mod }, key = "Left", action = "workspace_prev" },
        { mods = { "Ctrl", mod }, key = "1", action = "workspace", workspace = 1 },
        { mods = { "Ctrl", mod }, key = "2", action = "workspace", workspace = 2 },
        { mods = { "Ctrl", mod }, key = "3", action = "workspace", workspace = 3 },
        { mods = { "Ctrl", mod }, key = "4", action = "workspace", workspace = 4 },
        { mods = { "Ctrl", mod, "Shift" }, key = "1", action = "move_to_workspace", workspace = 1 },
        { mods = { "Ctrl", mod, "Shift" }, key = "2", action = "move_to_workspace", workspace = 2 },
        { mods = { "Ctrl", mod, "Shift" }, key = "3", action = "move_to_workspace", workspace = 3 },
        { mods = { "Ctrl", mod, "Shift" }, key = "4", action = "move_to_workspace", workspace = 4 },
        { mods = { mod, "Shift" }, key = "r", action = "reload" },
        { mods = { mod, "Shift" }, key = "Escape", action = "quit" },
    },
}
