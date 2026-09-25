// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>

namespace shaode {
struct ImportResult {
    std::string theme;  // Lua source for theme.lua, already validated
    std::string report; // what was imported from where, and what was not
    int imported = 0;   // settings in `theme`
};

// Reads the Hyprland, Waybar, wallbash, and pywal files under `directory` (a ~/.config, or a
// copy of one) and translates their look and hardware settings into a shaoDe theme. Throws
// when the directory has none of them.
ImportResult import_dotfiles(const std::filesystem::path &directory);
} // namespace shaode
