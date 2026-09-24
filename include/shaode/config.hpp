#pragma once

#include "shaode/backend.h"
#include <filesystem>
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

struct ShellConfig {
    bool enabled = true;
    int panel_height = 52;
    std::string accent = "#7da8ff";
    std::string panel_color = "#151e2c";
    std::string text_color = "#edf2fa";
    std::string wallpaper;
    std::vector<Launcher> launchers;
};

struct Config {
    sh_settings settings{
        {0.10F, 0.13F, 0.18F, 1.0F}, SH_ALT, 25, 600, 8, "us", "", true, 4, {}, 0, ""};
    std::vector<Binding> bindings;
    std::vector<Command> startup;
    ShellConfig shell;

    const Binding *binding(uint32_t modifiers, uint32_t keysym) const;
};

// Maps a Lua/control-socket action name; throws for unknown names.
sh_action parse_action(const std::string &name);
bool action_takes_workspace(sh_action action);

// Parse into a fresh value; callers replace the active configuration only on success.
Config load_config(const std::filesystem::path &path);
Config parse_config(const std::string &source, const std::string &name = "config");
} // namespace shaode
