-- SPDX-License-Identifier: GPL-3.0-or-later
-- shaoDe configuration, API version 1.
-- Launch commands are argument arrays, never shell strings.
-- Hyprland-style Super shortcuts. A nested session inside a host that grabs Super
-- (Hyprland, GNOME) never sees them; set mod = "Alt" there.
local mod = "Super"

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
        -- Per-monitor mode, scale, position, rotation (0-7), or enabled = false, e.g.:
        -- monitors = { ["DP-3"] = { mode = "2560x1440@200", scale = 1.25 } },
    },
    xwayland = true, -- run X11 applications; Xwayland starts on first use (restart to change)
    shell = {
        enabled = true,
        panel_height = 52,
        panel_position = "bottom", -- or "top"
        panel_margin = 0, -- or { top = 8, right = 12, bottom = 0, left = 12 } for a floating bar
        panel_radius = 0,
        font = "", -- family name; empty keeps the default
        font_size = 12,
        accent = "#7da8ff",
        panel_color = "#151e2c", -- #RRGGBBAA makes it translucent
        text_color = "#edf2fa",
        wallpaper = "", -- absolute path, or relative to this configuration file
        launchers = {
            { name = "Terminal", icon = "utilities-terminal", command = { "kitty" } },
            { name = "Home", icon = "user-home", command = { "xdg-open", "." } },
        },
    },
    startup = {}, -- e.g. { { "kitty" } }
    bindings = {
        { mods = { mod }, key = "q", action = "spawn", command = { "kitty" } },
        { mods = { mod }, key = "r", action = "launcher" },
        { mods = { mod }, key = "c", action = "close" },
        { mods = { mod }, key = "m", action = "quit" },
        { mods = { mod }, key = "v", action = "toggle_floating" },
        { mods = { mod }, key = "f", action = "fullscreen" },
        { mods = { mod }, key = "t", action = "tile" },
        { mods = { mod, "Shift" }, key = "t", action = "toggle_tiling" },
        { mods = { "Alt" }, key = "Tab", action = "cycle" },
        { mods = { mod }, key = "Left", action = "snap_left" },
        { mods = { mod }, key = "Right", action = "snap_right" },
        { mods = { mod }, key = "Up", action = "maximize" },
        { mods = { mod }, key = "Down", action = "restore" },
        -- Lock with any ext-session-lock client, e.g.:
        -- { mods = { mod }, key = "l", action = "spawn", command = { "swaylock" } },
        { mods = { mod }, key = "1", action = "workspace", workspace = 1 },
        { mods = { mod }, key = "2", action = "workspace", workspace = 2 },
        { mods = { mod }, key = "3", action = "workspace", workspace = 3 },
        { mods = { mod }, key = "4", action = "workspace", workspace = 4 },
        { mods = { mod, "Shift" }, key = "1", action = "move_to_workspace", workspace = 1 },
        { mods = { mod, "Shift" }, key = "2", action = "move_to_workspace", workspace = 2 },
        { mods = { mod, "Shift" }, key = "3", action = "move_to_workspace", workspace = 3 },
        { mods = { mod, "Shift" }, key = "4", action = "move_to_workspace", workspace = 4 },
        { mods = { "Ctrl", mod }, key = "Right", action = "workspace_next" },
        { mods = { "Ctrl", mod }, key = "Left", action = "workspace_prev" },
        { mods = { mod, "Shift" }, key = "r", action = "reload" },
    },
}
