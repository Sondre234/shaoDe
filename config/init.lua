-- SPDX-License-Identifier: GPL-3.0-or-later
-- shaoDe configuration, API version 1.
-- Launch commands are argument arrays, never shell strings.
-- Hyprland-style Super shortcuts. A nested session inside a host that grabs Super
-- (Hyprland, GNOME) never sees them; set mod = "Alt" there.
local mod = "Super"

return {
    version = 1,
    -- `shaode import ~/.config` writes theme.lua from Hyprland, Waybar, wallbash, or pywal
    -- files. It fills in whatever this file leaves out, so the look settings below are
    -- comments showing their defaults; set one here to override the import.
    theme = "theme.lua",
    -- appearance = { background = "#19212e" },
    -- keyboard = { layout = "us", options = "", repeat_rate = 25, repeat_delay = 600 },
    mouse = {
        modifier = mod, -- modifier + left drag moves; right drag resizes
        -- focus_follows = true, -- hovering a window focuses it (without raising it)
        -- Standalone sessions only; unset keeps each device's default:
        -- speed = 0.0 (-1 to 1), acceleration = "flat" or "adaptive", natural_scroll = false
    },
    -- touchpad = { natural_scroll = true, tap_to_click = true, disable_while_typing = true },
    layout = {
        -- gap = 8, -- sets both; or gap_inner (between windows) and gap_outer (at the edges)
        workspaces = 4,
        tiling = false, -- start with automatic tiling; the panel button toggles it
    },
    windows = {
        -- border_width = 0, -- drawn around each window; tiles shrink to keep it in their slot
        -- border_color = "#7da8ff", -- the focused window; #RRGGBBAA also works
        -- border_inactive_color = "#404a5c",
        -- opacity = 1.0,
        -- inactive_opacity = 1.0,
        -- Per application, by app ID (a regular expression); the first match wins:
        -- rules = { { app_id = "^firefox$", opacity = 0.9, inactive_opacity = 0.85 } },
    },
    outputs = {
        -- Left to right by connector name; unlisted monitors follow on the right.
        order = { "HDMI-A-1", "DP-3", "DP-1" },
        primary = "DP-3", -- the cursor starts here; without it, the leftmost monitor
        -- Per-monitor mode, scale, position, rotation (0-7), vrr, or enabled = false, keyed
        -- by connector or by "desc:" and the start of "make model serial", e.g.:
        -- monitors = { ["DP-3"] = { mode = "2560x1440@200", scale = 1.25 } },
    },
    screenshots = {
        -- Saved as Screenshot_<date>_<time>.png; "~/" means your home directory. Empty or unset:
        -- $XDG_PICTURES_DIR/Screenshots, else ~/Pictures/Screenshots.
        -- directory = "~/Pictures/Screenshots",
        clipboard = true, -- also copy the image (needs wl-copy)
        notify = true, -- announce it with notify-send, when that is installed
    },
    xwayland = true, -- run X11 applications; Xwayland starts on first use (restart to change)
    shell = {
        enabled = true,
        -- panel_height = 52,
        -- panel_position = "bottom", -- or "top"
        -- panel_margin = 0, -- or { top = 8, right = 12, bottom = 0, left = 12 } to float
        -- panel_radius = 0,
        -- font = "", -- family name; empty keeps the default
        -- font_size = 12,
        -- accent = "#7da8ff",
        -- panel_color = "#151e2c", -- #RRGGBBAA makes it translucent
        -- text_color = "#edf2fa",
        -- wallpaper = "", -- absolute path, or relative to this configuration file
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
        { mods = { mod }, key = "Left", action = "focus_left" },
        { mods = { mod }, key = "Right", action = "focus_right" },
        { mods = { mod }, key = "Up", action = "focus_up" },
        { mods = { mod }, key = "Down", action = "focus_down" },
        { mods = { mod, "Shift" }, key = "Left", action = "snap_left" },
        { mods = { mod, "Shift" }, key = "Right", action = "snap_right" },
        { mods = { mod, "Shift" }, key = "Up", action = "maximize" },
        { mods = { mod, "Shift" }, key = "Down", action = "restore" },
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
        -- Screenshots run grim (and slurp to select a region).
        { mods = {}, key = "Print", action = "screenshot", mode = "region" },
        { mods = { "Shift" }, key = "Print", action = "screenshot", mode = "output" },
        { mods = { mod }, key = "Print", action = "screenshot", mode = "window" },
    },
}
