// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaode/config.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <lua.hpp>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <xkbcommon/xkbcommon.h>

namespace shaode {
namespace {
using State = std::unique_ptr<lua_State, decltype(&lua_close)>;
[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error("configuration: " + message);
}
void table(lua_State *L, int index, const char *label) {
    if (!lua_istable(L, index))
        fail(std::string(label) + " must be a table");
}
void keys(lua_State *L, int index, std::initializer_list<std::string_view> allowed) {
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            fail("expected a named setting");
        std::string key = lua_tostring(L, -2);
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            fail("unknown setting '" + key + "'");
        lua_pop(L, 1);
    }
}
std::string string(lua_State *L, int index, const char *label) {
    if (lua_type(L, index) != LUA_TSTRING)
        fail(std::string(label) + " must be a string");
    size_t length = 0;
    const char *value = lua_tolstring(L, index, &length);
    if (length > 4096 || std::memchr(value, '\0', length))
        fail(std::string(label) + " is too long or contains a NUL byte");
    return {value, length};
}
std::string field(lua_State *L, const char *key) {
    lua_getfield(L, -1, key);
    auto result = string(L, -1, key);
    lua_pop(L, 1);
    return result;
}
int integer(lua_State *L, const char *key, int fallback, int min, int max) {
    lua_getfield(L, -1, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return fallback;
    }
    if (!lua_isinteger(L, -1))
        fail(std::string(key) + " must be an integer");
    auto result = lua_tointeger(L, -1);
    if (result < min || result > max)
        fail(std::string(key) + " is out of range");
    lua_pop(L, 1);
    return static_cast<int>(result);
}
template <std::size_t N>
void copy_text(const std::string &value, char (&target)[N], const std::string &label) {
    if (value.size() >= N)
        fail(label + " is too long");
    std::memcpy(target, value.c_str(), value.size() + 1);
}
template <std::size_t N> void text_field(lua_State *L, const char *key, char (&target)[N]) {
    lua_getfield(L, -1, key);
    if (!lua_isnil(L, -1))
        copy_text(string(L, -1, key), target, key);
    lua_pop(L, 1);
}
void boolean(lua_State *L, const char *key, const char *label, bool &target) {
    lua_getfield(L, -1, key);
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1))
            fail(std::string(label) + " must be a boolean");
        target = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
}
// A boolean that may be left unset (-1) to keep a device default.
void tristate(lua_State *L, const char *key, const char *label, int &target) {
    bool value = false;
    lua_getfield(L, -1, key);
    bool present = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (!present)
        return;
    boolean(L, key, label, value);
    target = value;
}
bool is_color(const std::string &value, bool alpha = false) {
    return (value.size() == 7 || (alpha && value.size() == 9)) && value[0] == '#' &&
           value.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos;
}
// #RRGGBB or #RRGGBBAA as premultiplied RGBA.
void premultiplied(const std::string &value, const char *label, float (&target)[4]) {
    if (!is_color(value, true))
        fail(std::string(label) + " must be #RRGGBB or #RRGGBBAA");
    float alpha = value.size() == 9 ? std::stoi(value.substr(7, 2), nullptr, 16) / 255.0F : 1.0F;
    for (size_t i = 0; i < 3; ++i)
        target[i] = std::stoi(value.substr(1 + 2 * i, 2), nullptr, 16) / 255.0F * alpha;
    target[3] = alpha;
}
// Pushes the optional table `name`, checking its keys; returns false when it is absent. The
// caller pops it either way.
bool section(lua_State *L, const char *name, std::initializer_list<std::string_view> allowed) {
    lua_getfield(L, -1, name);
    if (lua_isnil(L, -1))
        return false;
    table(L, -1, name);
    keys(L, -1, allowed);
    return true;
}
size_t array_size(lua_State *L, int index, size_t limit) {
    table(L, index, "list");
    index = lua_absindex(L, index);
    auto size = lua_rawlen(L, index);
    if (size > limit)
        fail("list is too long");
    size_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 ||
            static_cast<size_t>(lua_tointeger(L, -2)) > size)
            fail("lists must have consecutive integer keys starting at 1");
        ++count;
        lua_pop(L, 1);
    }
    if (count != size)
        fail("list contains holes");
    return size;
}
uint32_t modifier(const std::string &name) {
    for (auto [candidate, bit] :
         {std::pair{"Alt", SH_ALT}, {"Super", SH_LOGO}, {"Ctrl", SH_CTRL}, {"Shift", SH_SHIFT}})
        if (name == candidate)
            return bit;
    fail("unknown modifier '" + name + "'");
}
Command command(lua_State *L) {
    auto size = array_size(L, -1, 256);
    if (size == 0)
        fail("command must include an executable");
    Command result;
    for (size_t i = 1; i <= size; ++i) {
        lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
        result.push_back(string(L, -1, "command argument"));
        lua_pop(L, 1);
    }
    if (result.front().empty())
        fail("command executable is empty");
    return result;
}
void read_shell(lua_State *L, ShellConfig &shell) {
    if (!section(L, "shell",
                 {"enabled", "panel_height", "panel_position", "panel_margin", "panel_radius",
                  "font", "font_size", "accent", "panel_color", "text_color", "wallpaper",
                  "launchers"})) {
        lua_pop(L, 1);
        return;
    }
    boolean(L, "enabled", "shell.enabled", shell.enabled);
    shell.panel_height = integer(L, "panel_height", 52, 24, 100);
    lua_getfield(L, -1, "panel_position");
    if (!lua_isnil(L, -1)) {
        auto position = string(L, -1, "panel_position");
        if (position != "top" && position != "bottom")
            fail("panel_position must be \"top\" or \"bottom\"");
        shell.panel_top = position == "top";
    }
    lua_pop(L, 1);
    // One number for every side, or { top, right, bottom, left } by name.
    lua_getfield(L, -1, "panel_margin");
    if (lua_isinteger(L, -1)) {
        auto margin = lua_tointeger(L, -1);
        if (margin < 0 || margin > 200)
            fail("panel_margin is out of range");
        for (auto &side : shell.panel_margin)
            side = static_cast<int>(margin);
    } else if (!lua_isnil(L, -1)) {
        table(L, -1, "panel_margin");
        keys(L, -1, {"top", "right", "bottom", "left"});
        int index = 0;
        for (const char *side : {"top", "right", "bottom", "left"})
            shell.panel_margin[index++] = integer(L, side, 0, 0, 200);
    }
    lua_pop(L, 1);
    shell.panel_radius = integer(L, "panel_radius", 0, 0, 50);
    lua_getfield(L, -1, "font");
    if (!lua_isnil(L, -1))
        shell.font = string(L, -1, "font");
    lua_pop(L, 1);
    shell.font_size = integer(L, "font_size", 12, 6, 48);
    for (auto [key, target] : {std::pair{"accent", &shell.accent},
                               {"panel_color", &shell.panel_color},
                               {"text_color", &shell.text_color},
                               {"wallpaper", &shell.wallpaper}}) {
        lua_getfield(L, -1, key);
        if (!lua_isnil(L, -1)) {
            *target = string(L, -1, key);
            if (target != &shell.wallpaper && !is_color(*target, true))
                fail(std::string(key) + " must be #RRGGBB or #RRGGBBAA");
        }
        lua_pop(L, 1);
    }
    lua_getfield(L, -1, "launchers");
    if (!lua_isnil(L, -1)) {
        auto size = array_size(L, -1, 64);
        for (size_t i = 1; i <= size; ++i) {
            lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
            table(L, -1, "launcher");
            keys(L, -1, {"name", "icon", "command"});
            Launcher launcher;
            launcher.name = field(L, "name");
            if (launcher.name.empty() || launcher.name.size() > 128)
                fail("launcher name must have 1 to 128 bytes");
            lua_getfield(L, -1, "icon");
            launcher.icon = lua_isnil(L, -1) ? "application-x-executable" : string(L, -1, "icon");
            lua_pop(L, 1);
            lua_getfield(L, -1, "command");
            launcher.command = command(L);
            lua_pop(L, 1);
            shell.launchers.push_back(std::move(launcher));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 2);
}
double number(lua_State *L, const char *key, double fallback, double min, double max) {
    lua_getfield(L, -1, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return fallback;
    }
    if (!lua_isnumber(L, -1))
        fail(std::string(key) + " must be a number");
    auto result = lua_tonumber(L, -1);
    if (!(result >= min && result <= max))
        fail(std::string(key) + " is out of range");
    lua_pop(L, 1);
    return result;
}
// "WIDTHxHEIGHT" or "WIDTHxHEIGHT@HZ", where HZ may have decimals.
void parse_mode(const std::string &mode, sh_monitor &monitor) {
    int width = 0, height = 0, used = 0;
    double hz = 0;
    auto *text = mode.c_str();
    bool valid = std::sscanf(text, "%5dx%5d%n", &width, &height, &used) == 2;
    if (valid && text[used] == '@') {
        int more = 0;
        valid = std::sscanf(text + used + 1, "%lf%n", &hz, &more) == 1 && hz >= 1 && hz <= 1000;
        used += 1 + more;
    }
    if (!valid || text[used] != '\0' || width < 1 || height < 1 || width > 16384 || height > 16384)
        fail("mode must be WIDTHxHEIGHT or WIDTHxHEIGHT@HZ, e.g. 2560x1440@144");
    monitor.width = width;
    monitor.height = height;
    monitor.refresh = static_cast<int>(hz * 1000 + 0.5);
}
void read_monitors(lua_State *L, sh_settings &settings) {
    lua_getfield(L, -1, "monitors");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    table(L, -1, "outputs.monitors");
    int index = lua_absindex(L, -1);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            fail("outputs.monitors is keyed by output name, e.g. [\"DP-1\"] = { ... }");
        if (settings.monitor_count == static_cast<int>(std::size(settings.monitors)))
            fail("outputs.monitors has too many entries");
        auto &monitor = settings.monitors[settings.monitor_count++];
        auto name = string(L, -2, "output name");
        if (name.empty())
            fail("output name is empty");
        copy_text(name, monitor.name, "output name");
        table(L, -1, "monitor settings");
        keys(L, -1, {"enabled", "mode", "scale", "position", "transform", "vrr", "tiling"});
        monitor.enabled = true;
        boolean(L, "enabled", "enabled", monitor.enabled);
        monitor.tiling = -1;
        tristate(L, "tiling", "tiling", monitor.tiling);
        lua_getfield(L, -1, "mode");
        if (!lua_isnil(L, -1))
            parse_mode(string(L, -1, "mode"), monitor);
        lua_pop(L, 1);
        monitor.scale = static_cast<float>(number(L, "scale", 0, 0.25, 10));
        monitor.transform = integer(L, "transform", 0, 0, 7);
        boolean(L, "vrr", "vrr", monitor.vrr);
        lua_getfield(L, -1, "position");
        if (!lua_isnil(L, -1)) {
            table(L, -1, "position");
            keys(L, -1, {"x", "y"});
            monitor.positioned = true;
            constexpr int unset = 1 << 30;
            monitor.x = integer(L, "x", unset, -65536, 65536);
            monitor.y = integer(L, "y", unset, -65536, 65536);
            if (monitor.x == unset || monitor.y == unset)
                fail("position needs both x and y");
        }
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
}
void read_windows(lua_State *L, Config &config) {
    if (!section(L, "windows",
                 {"border_width", "border_color", "border_inactive_color", "opacity",
                  "inactive_opacity", "rules", "buttons"})) {
        lua_pop(L, 1);
        return;
    }
    config.settings.border_width = integer(L, "border_width", 0, 0, 20);
    for (auto [key, target] : {std::pair{"border_color", &config.settings.border_active},
                               {"border_inactive_color", &config.settings.border_inactive}}) {
        lua_getfield(L, -1, key);
        if (!lua_isnil(L, -1))
            premultiplied(string(L, -1, key), key, *target);
        lua_pop(L, 1);
    }
    config.opacity = static_cast<float>(number(L, "opacity", 1, 0.05, 1));
    config.inactive_opacity =
        static_cast<float>(number(L, "inactive_opacity", config.opacity, 0.05, 1));
    lua_getfield(L, -1, "buttons");
    if (!lua_isnil(L, -1)) {
        config.window_buttons = string(L, -1, "windows.buttons");
        if (config.window_buttons.find_first_not_of("abcdefghijklmnopqrstuvwxyz_,:") !=
            std::string::npos)
            fail("windows.buttons must look like \"appmenu:minimize,maximize,close\"");
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "rules");
    if (!lua_isnil(L, -1)) {
        auto size = array_size(L, -1, 256);
        for (size_t i = 1; i <= size; ++i) {
            lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
            table(L, -1, "window rule");
            keys(L, -1, {"app_id", "opacity", "inactive_opacity"});
            WindowRule rule;
            rule.app_id = field(L, "app_id");
            try {
                rule.pattern = std::regex(rule.app_id, std::regex::ECMAScript);
            } catch (const std::regex_error &) {
                fail("app_id '" + rule.app_id + "' is not a valid regular expression");
            }
            rule.opacity = static_cast<float>(number(L, "opacity", 1, 0.05, 1));
            rule.inactive_opacity =
                static_cast<float>(number(L, "inactive_opacity", rule.opacity, 0.05, 1));
            config.window_rules.push_back(std::move(rule));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 2);
}
void instruction_limit(lua_State *L, lua_Debug *) {
    auto *remaining = static_cast<int *>(lua_getextraspace(L));
    if (--*remaining <= 0)
        luaL_error(L, "configuration exceeded its instruction budget");
}
Config read(lua_State *L, size_t own = SIZE_MAX) {
    Config config;
    table(L, -1, "configuration result");
    keys(L, -1,
         {"version", "extends", "theme", "appearance", "keyboard", "mouse", "touchpad", "layout", "outputs",
          "windows", "animations", "bindings", "startup", "shell", "xwayland", "screenshots"});
    read_shell(L, config.shell);
    if (integer(L, "version", 1, 1, 1) != 1)
        fail("unsupported version");
    boolean(L, "xwayland", "xwayland", config.settings.xwayland);
    if (section(L, "appearance", {"background"})) {
        auto color = field(L, "background");
        if (!is_color(color))
            fail("background must be #RRGGBB");
        for (size_t i = 0; i < 3; ++i)
            config.settings.background[i] =
                std::stoi(color.substr(1 + 2 * i, 2), nullptr, 16) / 255.0F;
    }
    lua_pop(L, 1);
    if (section(L, "keyboard", {"layout", "options", "repeat_rate", "repeat_delay"})) {
        text_field(L, "layout", config.settings.keyboard_layout);
        text_field(L, "options", config.settings.keyboard_options);
        config.settings.repeat_rate = integer(L, "repeat_rate", 25, 0, 100);
        config.settings.repeat_delay = integer(L, "repeat_delay", 600, 0, 5000);
    }
    lua_pop(L, 1);
    if (section(L, "mouse",
                {"modifier", "speed", "acceleration", "natural_scroll", "focus_follows"})) {
        lua_getfield(L, -1, "modifier");
        if (!lua_isnil(L, -1))
            config.settings.mouse_modifier = modifier(string(L, -1, "modifier"));
        lua_pop(L, 1);
        lua_getfield(L, -1, "speed");
        config.settings.pointer_speed_set = !lua_isnil(L, -1);
        lua_pop(L, 1);
        config.settings.pointer_speed = number(L, "speed", 0, -1, 1);
        lua_getfield(L, -1, "acceleration");
        if (!lua_isnil(L, -1)) {
            auto profile = string(L, -1, "acceleration");
            if (profile != "flat" && profile != "adaptive")
                fail("mouse.acceleration must be \"flat\" or \"adaptive\"");
            config.settings.pointer_accel = profile == "adaptive";
        }
        lua_pop(L, 1);
        tristate(L, "natural_scroll", "mouse.natural_scroll", config.settings.mouse_natural_scroll);
        boolean(L, "focus_follows", "mouse.focus_follows", config.settings.focus_follows_mouse);
    }
    lua_pop(L, 1);
    if (section(L, "touchpad", {"natural_scroll", "tap_to_click", "disable_while_typing"})) {
        tristate(L, "natural_scroll", "touchpad.natural_scroll",
                 config.settings.touchpad_natural_scroll);
        tristate(L, "tap_to_click", "touchpad.tap_to_click", config.settings.touchpad_tap);
        tristate(L, "disable_while_typing", "touchpad.disable_while_typing",
                 config.settings.touchpad_dwt);
    }
    lua_pop(L, 1);
    if (section(L, "layout", {"gap", "gap_inner", "gap_outer", "workspaces", "tiling"})) {
        // gap sets both; gap_inner and gap_outer override it.
        int gap = integer(L, "gap", 8, 0, 100);
        config.settings.gap_inner = integer(L, "gap_inner", gap, 0, 100);
        config.settings.gap_outer = integer(L, "gap_outer", gap, 0, 100);
        boolean(L, "tiling", "layout.tiling", config.settings.tiling);
        config.settings.workspaces = integer(L, "workspaces", 4, 1, 10);
    }
    lua_pop(L, 1);
    if (section(L, "outputs", {"order", "primary", "monitors"})) {
        read_monitors(L, config.settings);
        lua_getfield(L, -1, "order");
        if (!lua_isnil(L, -1)) {
            auto size = array_size(L, -1, std::size(config.settings.output_order));
            for (size_t i = 1; i <= size; ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
                auto name = string(L, -1, "output name");
                if (name.empty())
                    fail("output name is empty");
                for (size_t j = 0; j + 1 < i; ++j)
                    if (name == config.settings.output_order[j])
                        fail("duplicate output '" + name + "'");
                copy_text(name, config.settings.output_order[i - 1], "output name");
                lua_pop(L, 1);
            }
            config.settings.output_count = static_cast<int>(size);
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "primary");
        if (!lua_isnil(L, -1)) {
            auto name = string(L, -1, "primary");
            if (name.empty())
                fail("primary output name is empty");
            copy_text(name, config.settings.primary_output, "primary output name");
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    read_windows(L, config);
    if (section(L, "animations", {"enabled", "duration"})) {
        boolean(L, "enabled", "animations.enabled", config.settings.animations);
        config.settings.animation_duration = integer(L, "duration", 120, 10, 1000);
    }
    lua_pop(L, 1);
    if (section(L, "screenshots", {"directory", "clipboard", "notify"})) {
        lua_getfield(L, -1, "directory");
        if (!lua_isnil(L, -1)) {
            auto directory = string(L, -1, "screenshots.directory");
            if (!directory.starts_with('/') && !directory.starts_with("~/"))
                fail("screenshots.directory must be absolute or start with ~/");
            config.screenshots.directory = directory;
        }
        lua_pop(L, 1);
        boolean(L, "clipboard", "screenshots.clipboard", config.screenshots.clipboard);
        boolean(L, "notify", "screenshots.notify", config.screenshots.notify);
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "bindings");
    if (!lua_isnil(L, -1)) {
        auto size = array_size(L, -1, 512);
        for (size_t i = 1; i <= size; ++i) {
            lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
            table(L, -1, "binding");
            keys(L, -1, {"mods", "key", "action", "command", "workspace", "mode"});
            Binding binding{};
            auto key = field(L, "key");
            binding.keysym =
                xkb_keysym_to_lower(xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_NO_FLAGS));
            if (binding.keysym == XKB_KEY_NoSymbol)
                fail("unknown key '" + key + "'");
            auto action = field(L, "action");
            binding.action = action == "none" ? SH_NONE : parse_action(action);
            lua_getfield(L, -1, "mods");
            auto mods = array_size(L, -1, 4);
            for (size_t j = 1; j <= mods; ++j) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(j));
                auto bit = modifier(string(L, -1, "modifier"));
                if (binding.modifiers & bit)
                    fail("duplicate modifier");
                binding.modifiers |= bit;
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            lua_getfield(L, -1, "command");
            if (binding.action == SH_HANDLED)
                binding.command = command(L);
            else if (!lua_isnil(L, -1))
                fail("command is only valid with spawn");
            lua_pop(L, 1);
            if (action_takes_workspace(binding.action)) {
                binding.workspace = integer(L, "workspace", 0, 1, config.settings.workspaces);
                if (binding.workspace == 0)
                    fail("workspace actions need a workspace number");
            } else {
                lua_getfield(L, -1, "workspace");
                if (!lua_isnil(L, -1))
                    fail("workspace is only valid with workspace actions");
                lua_pop(L, 1);
            }
            lua_getfield(L, -1, "mode");
            if (!lua_isnil(L, -1)) {
                if (binding.action != SH_SCREENSHOT)
                    fail("mode is only valid with screenshot");
                binding.screenshot = parse_screenshot_mode(string(L, -1, "mode"));
            }
            lua_pop(L, 1);
            // Bindings past `own` come from the defaults a configuration extends; its own
            // bindings, "none" included, take their keys first.
            if (!config.binding(binding.modifiers, binding.keysym))
                config.bindings.push_back(std::move(binding));
            else if (i <= own)
                fail("duplicate keyboard binding");
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    std::erase_if(config.bindings,
                  [](const Binding &binding) { return binding.action == SH_NONE; });
    lua_getfield(L, -1, "startup");
    if (!lua_isnil(L, -1)) {
        auto size = array_size(L, -1, 32);
        for (size_t i = 1; i <= size; ++i) {
            lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
            config.startup.push_back(command(L));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    std::unique_ptr<xkb_context, decltype(&xkb_context_unref)> context(
        xkb_context_new(XKB_CONTEXT_NO_FLAGS), xkb_context_unref);
    if (!context)
        fail("cannot create XKB context");
    xkb_rule_names names{};
    names.layout = config.settings.keyboard_layout;
    names.options = config.settings.keyboard_options;
    std::unique_ptr<xkb_keymap, decltype(&xkb_keymap_unref)> keymap(
        xkb_keymap_new_from_names(context.get(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS),
        xkb_keymap_unref);
    if (!keymap)
        fail("invalid keyboard layout/options");
    return config;
}
} // namespace

sh_action parse_action(const std::string &name) {
    static constexpr std::pair<std::string_view, sh_action> actions[] = {
        {"spawn", SH_HANDLED},
        {"quit", SH_QUIT},
        {"close", SH_CLOSE},
        {"cycle", SH_CYCLE},
        {"snap_left", SH_SNAP_LEFT},
        {"snap_right", SH_SNAP_RIGHT},
        {"maximize", SH_MAXIMIZE},
        {"restore", SH_RESTORE},
        {"tile", SH_TILE},
        {"reload", SH_RELOAD},
        {"fullscreen", SH_FULLSCREEN},
        {"workspace", SH_WORKSPACE},
        {"move_to_workspace", SH_MOVE_TO_WORKSPACE},
        {"workspace_next", SH_WORKSPACE_NEXT},
        {"workspace_prev", SH_WORKSPACE_PREV},
        {"toggle_tiling", SH_TOGGLE_TILING},
        {"toggle_floating", SH_TOGGLE_FLOATING},
        {"launcher", SH_LAUNCHER},
        {"focus_left", SH_FOCUS_LEFT},
        {"focus_right", SH_FOCUS_RIGHT},
        {"focus_up", SH_FOCUS_UP},
        {"focus_down", SH_FOCUS_DOWN},
        {"screenshot", SH_SCREENSHOT},
    };
    for (const auto &[candidate, action] : actions)
        if (name == candidate)
            return action;
    fail("unknown action '" + name + "'");
}

bool action_takes_workspace(sh_action action) {
    return action == SH_WORKSPACE || action == SH_MOVE_TO_WORKSPACE;
}

sh_screenshot_mode parse_screenshot_mode(const std::string &name) {
    for (auto [candidate, mode] : {std::pair{"region", SH_SCREENSHOT_REGION},
                                   {"output", SH_SCREENSHOT_OUTPUT},
                                   {"window", SH_SCREENSHOT_WINDOW}})
        if (name == candidate)
            return mode;
    fail("screenshot mode must be \"region\", \"output\", or \"window\"");
}

float Config::window_opacity(const std::string &app_id, bool active) const {
    for (const auto &rule : window_rules)
        if (std::regex_search(app_id, rule.pattern))
            return active ? rule.opacity : rule.inactive_opacity;
    return active ? opacity : inactive_opacity;
}

const Binding *Config::binding(uint32_t modifiers, uint32_t keysym) const {
    constexpr uint32_t relevant = SH_SHIFT | SH_CTRL | SH_ALT | SH_LOGO;
    modifiers &= relevant; // CapsLock and NumLock do not disable shortcuts.
    keysym = xkb_keysym_to_lower(keysym);
    for (const auto &binding : bindings)
        if (binding.modifiers == modifiers && binding.keysym == keysym)
            return &binding;
    return nullptr;
}

namespace {
State sandbox() {
    State state(luaL_newstate(), lua_close);
    if (!state)
        fail("cannot allocate Lua state");
    auto *L = state.get();
    // Configuration can compute values but cannot perform I/O or launch processes.
    const std::pair<const char *, lua_CFunction> libraries[] = {{"_G", luaopen_base},
                                                                {LUA_TABLIBNAME, luaopen_table},
                                                                {LUA_STRLIBNAME, luaopen_string},
                                                                {LUA_MATHLIBNAME, luaopen_math},
                                                                {LUA_UTF8LIBNAME, luaopen_utf8}};
    for (const auto &[name, open] : libraries) {
        luaL_requiref(L, name, open, 1);
        lua_pop(L, 1);
    }
    for (const char *name : {"dofile", "loadfile", "load", "print", "collectgarbage",
                             "setmetatable", "getmetatable"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    lua_sethook(L, instruction_limit, LUA_MASKCOUNT, 1000);
    return state;
}
// Runs one chunk and leaves its single result on the stack.
void evaluate(lua_State *L, const std::string &source, const std::string &name) {
    *static_cast<int *>(lua_getextraspace(L)) = 1000;
    if (luaL_loadbufferx(L, source.data(), source.size(), name.c_str(), "t") != LUA_OK ||
        lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        fail(message ? message : "Lua raised a non-string error");
    }
}
std::string read_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        fail("cannot open " + path.string());
    std::string source;
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
        source.append(buffer, static_cast<size_t>(file.gcount()));
        if (source.size() > 1024 * 1024)
            fail("file exceeds 1 MiB");
    }
    if (file.bad())
        fail("cannot read " + path.string());
    return source;
}
bool is_record(lua_State *L, int index) {
    return lua_istable(L, index) && lua_rawlen(L, index) == 0;
}
// Copies what `target` lacks from `source`, descending into tables both have as records; lists
// and values `target` already sets stay as they are.
void merge(lua_State *L, int target, int source, int depth = 0) {
    if (depth > 16)
        fail("theme tables are nested too deeply");
    target = lua_absindex(L, target);
    source = lua_absindex(L, source);
    lua_pushnil(L);
    while (lua_next(L, source)) {
        lua_pushvalue(L, -2);
        lua_rawget(L, target);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, target);
        } else {
            if (is_record(L, -1) && is_record(L, -2))
                merge(L, -1, -2, depth + 1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}
std::filesystem::path theme_path(lua_State *L, const std::filesystem::path &directory) {
    lua_getfield(L, -1, "theme");
    std::filesystem::path path;
    if (!lua_isnil(L, -1))
        path = directory / string(L, -1, "theme");
    lua_pop(L, 1);
    return path;
}
// `theme = "theme.lua"` fills in every setting the configuration leaves out. A missing theme
// file is not an error, so a configuration can name one before `shaode import` writes it.
void include_theme(lua_State *L, const std::filesystem::path &directory) {
    table(L, -1, "configuration result");
    auto path = theme_path(L, directory);
    if (path.empty() || !std::filesystem::exists(path))
        return;
    evaluate(L, read_file(path), "@" + path.string());
    table(L, -1, "theme result");
    lua_getfield(L, -1, "theme");
    if (!lua_isnil(L, -1))
        fail("a theme cannot include another theme");
    lua_pop(L, 1);
    merge(L, -2, -1);
    lua_pop(L, 1);
}
// `extends = "default"` layers the configuration, theme included, over the shipped default
// configuration: every setting it leaves out comes from there, its bindings go first, and the
// defaults fill in the keys it does not bind. Returns how many bindings are its own.
size_t include_defaults(lua_State *L) {
    lua_getfield(L, -1, "extends");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return SIZE_MAX;
    }
    if (string(L, -1, "extends") != "default")
        fail("extends must be \"default\"");
    lua_pop(L, 1);
    const auto *variable = std::getenv("SHAODE_DEFAULT_CONFIG");
    std::filesystem::path path = variable && *variable ? variable : SHAODE_DEFAULT_CONFIG;
    evaluate(L, read_file(path), "@" + path.string());
    table(L, -1, "default configuration result");
    lua_getfield(L, -1, "extends");
    if (!lua_isnil(L, -1))
        fail("the default configuration cannot extend another");
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setfield(L, -2, "theme");
    lua_getfield(L, -2, "bindings");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -4, "bindings");
    }
    table(L, -1, "bindings");
    size_t own = lua_rawlen(L, -1);
    lua_getfield(L, -2, "bindings");
    if (!lua_isnil(L, -1)) {
        table(L, -1, "default bindings");
        for (lua_Integer i = 1; i <= static_cast<lua_Integer>(lua_rawlen(L, -1)); ++i) {
            lua_rawgeti(L, -1, i);
            lua_rawseti(L, -3, static_cast<lua_Integer>(own) + i);
        }
    }
    lua_pop(L, 2);
    merge(L, -2, -1);
    lua_pop(L, 1);
    return own;
}
void shadowed(lua_State *L, int config, int theme, const std::string &prefix,
              std::vector<std::string> &result) {
    config = lua_absindex(L, config);
    theme = lua_absindex(L, theme);
    lua_pushnil(L);
    while (lua_next(L, theme)) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            auto name = prefix + lua_tostring(L, -2);
            lua_pushvalue(L, -2);
            lua_rawget(L, config);
            if (is_record(L, -1) && is_record(L, -2))
                shadowed(L, -1, -2, name + ".", result);
            else if (!lua_isnil(L, -1))
                result.push_back(name);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
}
} // namespace

Config parse_config(const std::string &source, const std::string &name,
                    const std::filesystem::path &directory) {
    auto state = sandbox();
    auto *L = state.get();
    evaluate(L, source, name);
    include_theme(L, directory);
    auto own = include_defaults(L);
    return read(L, own);
}

Config load_config(const std::filesystem::path &path) {
    return parse_config(read_file(path), "@" + path.string(), path.parent_path());
}

std::optional<std::vector<std::string>> shadowed_settings(const std::filesystem::path &config) {
    auto state = sandbox();
    auto *L = state.get();
    evaluate(L, read_file(config), "@" + config.string());
    table(L, -1, "configuration result");
    auto path = theme_path(L, config.parent_path());
    if (path.empty())
        return std::nullopt;
    std::vector<std::string> result;
    if (!std::filesystem::exists(path))
        return result;
    evaluate(L, read_file(path), "@" + path.string());
    table(L, -1, "theme result");
    shadowed(L, -2, -1, "", result);
    std::sort(result.begin(), result.end());
    return result;
}
} // namespace shaode
