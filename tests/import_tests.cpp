// SPDX-License-Identifier: GPL-3.0-or-later
// `shaode import` against the fixture dotfiles in tests/import, and theme.lua merging.
#include "shaode/config.hpp"
#include "shaode/import.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
bool contains(const std::string &text, const std::string &part) {
    return text.find(part) != std::string::npos;
}
const sh_monitor *monitor(const shaode::Config &config, const std::string &name) {
    for (int i = 0; i < config.settings.monitor_count; ++i)
        if (name == config.settings.monitors[i].name)
            return &config.settings.monitors[i];
    return nullptr;
}
bool near(float a, float b) { return a > b - 0.005F && a < b + 0.005F; }

// hyprland.conf through `source` and $variables, Waybar as a plain bar, pywal colors.
void hyprlang(const fs::path &root) {
    auto result = shaode::import_dotfiles(root / "conf/.config");
    auto config = shaode::parse_config(result.theme, "theme.lua");
    const auto &s = config.settings;
    require(s.gap_inner == 6 && s.gap_outer == 5,
            "gaps: gaps_in doubles, gaps_out takes its first value");
    require(s.border_width == 2, "border_size not imported");
    // $accent's rgba(ff8800ee) is the first color of the gradient; borders are premultiplied.
    require(near(s.border_active[3], 0xee / 255.0F) && near(s.border_active[0], 0xee / 255.0F),
            "active border color not taken from the gradient's first color");
    require(near(s.border_inactive[3], 0xaa / 255.0F), "0xAARRGGBB border color not read");
    require(config.shell.accent == "#ff8800", "accent does not follow the active border");
    require(near(config.opacity, 0.95F), "active_opacity not imported");
    require(std::string(s.keyboard_layout) == "us" &&
                std::string(s.keyboard_options) == "caps:escape" && s.repeat_rate == 30,
            "keyboard settings not imported");
    require(s.pointer_speed_set && near(static_cast<float>(s.pointer_speed), -0.25F) &&
                s.pointer_accel == 0,
            "pointer settings not imported");
    require(s.touchpad_natural_scroll == 1 && s.touchpad_tap == 1,
            "touchpad settings not imported");

    auto *dp1 = monitor(config, "DP-1");
    require(dp1 && dp1->width == 2560 && dp1->refresh == 143970 && near(dp1->scale, 1.5F) &&
                dp1->positioned && dp1->x == 1920 && dp1->transform == 1 && dp1->vrr,
            "DP-1's monitor line not imported");
    auto *hdmi = monitor(config, "HDMI-A-1");
    require(hdmi && hdmi->width == 1920 && hdmi->refresh == 0,
            "a mode without a rate not imported");
    auto *dp2 = monitor(config, "DP-2");
    require(dp2 && !dp2->enabled, "a disabled monitor not imported");
    auto *dp9 = monitor(config, "DP-9");
    require(dp9 && dp9->scale == 0 && dp9->x == -1920, "a bad scale should drop only the scale");
    require(s.output_count == 3 && std::string(s.output_order[0]) == "DP-9" &&
                std::string(s.output_order[2]) == "DP-1",
            "output order does not follow positions");

    require(config.window_rules.size() == 4, "expected four opacity rules");
    require(near(config.window_opacity("kitty", true), 0.8F) &&
                near(config.window_opacity("kitty", false), 0.7F),
            "v1 opacity rule not imported");
    require(near(config.window_opacity("firefox", true), 0.9F) &&
                near(config.window_opacity("firefox-nightly", true), 0.95F),
            "v2 rule should match the whole class");
    require(near(config.window_opacity("foot", false), 0.85F), "match:class rule not imported");
    require(near(config.window_opacity("zen", false), 0.9F), "windowrule block not imported");

    require(config.shell.panel_color == "#1e1e2e80", "alpha(@base, 0.5) not resolved");
    require(config.shell.text_color == "#cdd6f4", "rgb() text color not resolved");
    require(config.shell.panel_radius == 6 && !config.shell.panel_top &&
                config.shell.panel_height == 30,
            "bar radius, position, or height not imported");
    const int margins[4] = {4, 8, 4, 8};
    require(std::equal(margins, margins + 4, config.shell.panel_margin),
            "margin shorthand not expanded");
    require(config.shell.font == "Fira Sans" && config.shell.font_size == 13, "font not imported");
    require(near(s.background[0], 0x10 / 255.0F), "pywal background not imported");

    const auto &report = result.report;
    for (const char *expected :
         {"rounded corners", "blur needs", "some-mouse", "source /etc/hostname", "scale 1ab",
          "matches more than the app ID", "key bindings (1)", "window rules without opacity (1)"})
        require(contains(report, expected), std::string("report lacks: ") + expected);
}

// hyprland.lua in the sandbox, a HyDE-style Waybar, wallbash colors and wallpaper.
void lua(const fs::path &root) {
    auto result = shaode::import_dotfiles(root / "lua/.config");
    auto config = shaode::parse_config(result.theme, "theme.lua");
    const auto &s = config.settings;
    auto *dp1 = monitor(config, "DP-1");
    require(dp1 && dp1->width == 3840 && near(dp1->scale, 2),
            "io.open of ~/.config/hypr/mode should read the copy and pick mode b");
    require(monitor(config, "DP-2") && !monitor(config, "DP-2")->enabled,
            "disabled = true not imported");
    require(s.gap_inner == 4, "gaps_in not imported from hl.config");
    require(s.gap_outer == 7, "require()d table not imported, or code after the error ran");
    require(config.shell.accent == "#e8a3c9", "gradient table's first color not used");
    require(near(s.border_inactive[3], 0xee / 255.0F), "integer ARGB color not read");
    require(near(config.inactive_opacity, 0.9F), "inactive_opacity not imported");
    require(s.touchpad_tap == 0, "[\"tap-to-click\"] not imported");
    require(config.window_rules.size() == 1 && near(config.window_opacity("kitty", false), 0.8F),
            "hl.window_rule not imported, or the disabled rule was");
    require(config.shell.panel_color == "#1c0d18cc" && config.shell.panel_radius == 16,
            "window#waybar > box look not preferred over a transparent window");
    require(config.shell.text_color == "#ffffff", "wallbash @define-color not resolved");
    require(config.shell.panel_top && config.shell.panel_margin[1] == 10 &&
                config.shell.panel_margin[0] == 0,
            "Waybar array config not read");
    require(near(s.background[0], 0x37 / 255.0F), "wallbash background not imported");
    require(fs::path(config.shell.wallpaper) == fs::canonical(root / "lua/wall.png"),
            "HyDE wallpaper link not followed");
    const auto &report = result.report;
    for (const char *expected :
         {"hyprland.lua:39: boom", "uses 'hyde'", "window rule is disabled", "event handlers (1)"})
        require(contains(report, expected), std::string("report lacks: ") + expected);
}

// init.lua's `theme` fills in only what init.lua leaves out.
void merge(const fs::path &root) {
    auto config = shaode::load_config(root / "merge/init.lua");
    require(config.shell.accent == "#111111", "init.lua should win over the theme");
    require(config.shell.text_color == "#333333", "theme should fill in what init.lua omits");
    require(config.settings.gap_inner == 3 && config.settings.gap_outer == 9,
            "nested tables should merge key by key");
    require(config.settings.border_width == 3, "theme-only section not merged");
    require(config.window_rules.size() == 1 && config.window_rules[0].app_id == "^a$",
            "lists should come whole from init.lua");
    auto shadowed = shaode::shadowed_settings(root / "merge/init.lua");
    require(shadowed && *shadowed == std::vector<std::string>{"layout.gap_inner", "shell.accent",
                                                              "windows.rules"},
            "shadowed settings wrong");
    auto missing = shaode::parse_config("return { theme = 'missing.lua', layout = { gap = 2 } }",
                                        "init.lua", root / "merge");
    require(missing.settings.gap_inner == 2, "a missing theme file should be ignored");
    bool nested = false;
    try {
        (void)shaode::parse_config("return { theme = 'init.lua' }", "init.lua", root / "merge");
    } catch (const std::exception &) {
        nested = true;
    }
    require(nested, "a theme that names another theme should be rejected");
    bool empty = false;
    try {
        (void)shaode::import_dotfiles(root / "merge");
    } catch (const std::exception &) {
        empty = true;
    }
    require(empty, "a directory without dotfiles should be an error");
}

int main(int argc, char **argv) {
    try {
        require(argc == 2, "fixture directory required");
        fs::path root = argv[1];
        hyprlang(root);
        lua(root);
        merge(root);
        std::cout << "import tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "import test failed: " << error.what() << '\n';
        return 1;
    }
}
