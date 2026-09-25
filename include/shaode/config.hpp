// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "shaode/backend.h"
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace shaode {
using Command = std::vector<std::string>;

struct Binding {
    uint32_t modifiers;
    uint32_t keysym;
    sh_action action;
    Command command;
    int workspace = 0; // for workspace and move_to_workspace, from 1
};

struct Launcher {
    std::string name;
    std::string icon;
    Command command;
};

// Windows whose app ID matches `app_id` (an ECMAScript regex, searched) get these opacities.
struct WindowRule {
    std::string app_id;
    std::regex pattern;
    float opacity = 1, inactive_opacity = 1;
};

struct ShellConfig {
    bool enabled = true;
    int panel_height = 52;
    bool panel_top = false;             // panel_position = "top"
    int panel_margin[4] = {0, 0, 0, 0}; // top, right, bottom, left: a floating bar
    int panel_radius = 0;
    std::string font;   // family; empty: the Qt default
    int font_size = 12; // taskbar text, in pixels
    std::string accent = "#7da8ff";
    std::string panel_color = "#151e2c";
    std::string text_color = "#edf2fa";
    std::string wallpaper;
    std::vector<Launcher> launchers;
};

struct Config {
    sh_settings settings{.background = {0.10F, 0.13F, 0.18F, 1.0F},
                         .mouse_modifier = SH_ALT,
                         .repeat_rate = 25,
                         .repeat_delay = 600,
                         .gap_inner = 8,
                         .gap_outer = 8,
                         .keyboard_layout = "us",
                         .keyboard_options = "",
                         .xwayland = true,
                         .tiling = false,
                         .workspaces = 4,
                         .output_order = {},
                         .output_count = 0,
                         .primary_output = "",
                         .monitors = {},
                         .monitor_count = 0,
                         .border_width = 0,
                         .border_active = {0.49F, 0.66F, 1.0F, 1.0F},
                         .border_inactive = {0.25F, 0.29F, 0.36F, 1.0F}};
    std::vector<Binding> bindings;
    std::vector<Command> startup;
    ShellConfig shell;
    float opacity = 1, inactive_opacity = 1;
    std::vector<WindowRule> window_rules;

    const Binding *binding(uint32_t modifiers, uint32_t keysym) const;
    // The first matching rule decides; otherwise the windows.opacity defaults.
    float window_opacity(const std::string &app_id, bool active) const;
};

// Maps a Lua/control-socket action name; throws for unknown names.
sh_action parse_action(const std::string &name);
bool action_takes_workspace(sh_action action);

// Parse into a fresh value; callers replace the active configuration only on success.
Config load_config(const std::filesystem::path &path);
Config parse_config(const std::string &source, const std::string &name = "config");
} // namespace shaode
