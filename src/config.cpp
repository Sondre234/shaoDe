#include "shaode/config.hpp"

#include <algorithm>
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
bool is_color(const std::string &value) {
    return value.size() == 7 && value[0] == '#' &&
           value.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos;
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
                 {"enabled", "panel_height", "accent", "panel_color", "text_color", "wallpaper",
                  "launchers"})) {
        lua_pop(L, 1);
        return;
    }
    boolean(L, "enabled", "shell.enabled", shell.enabled);
    shell.panel_height = integer(L, "panel_height", 52, 32, 100);
    for (auto [key, target] : {std::pair{"accent", &shell.accent},
                               {"panel_color", &shell.panel_color},
                               {"text_color", &shell.text_color},
                               {"wallpaper", &shell.wallpaper}}) {
        lua_getfield(L, -1, key);
        if (!lua_isnil(L, -1)) {
            *target = string(L, -1, key);
            if (target != &shell.wallpaper && !is_color(*target))
                fail(std::string(key) + " must be #RRGGBB");
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
void instruction_limit(lua_State *L, lua_Debug *) {
    auto *remaining = static_cast<int *>(lua_getextraspace(L));
    if (--*remaining <= 0)
        luaL_error(L, "configuration exceeded its instruction budget");
}
Config read(lua_State *L) {
    Config config;
    table(L, -1, "configuration result");
    keys(L, -1,
         {"version", "appearance", "keyboard", "mouse", "layout", "outputs", "bindings", "startup",
          "shell", "xwayland"});
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
    if (section(L, "mouse", {"modifier"})) {
        config.settings.mouse_modifier = modifier(field(L, "modifier"));
    }
    lua_pop(L, 1);
    if (section(L, "layout", {"gap", "workspaces", "tiling"})) {
        config.settings.gap = integer(L, "gap", 8, 0, 100);
        boolean(L, "tiling", "layout.tiling", config.settings.tiling);
        config.settings.workspaces = integer(L, "workspaces", 4, 1, 10);
    }
    lua_pop(L, 1);
    if (section(L, "outputs", {"order", "primary"})) {
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
    lua_getfield(L, -1, "bindings");
    if (!lua_isnil(L, -1)) {
        auto size = array_size(L, -1, 512);
        for (size_t i = 1; i <= size; ++i) {
            lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
            table(L, -1, "binding");
            keys(L, -1, {"mods", "key", "action", "command", "workspace"});
            Binding binding{};
            auto key = field(L, "key");
            binding.keysym =
                xkb_keysym_to_lower(xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_NO_FLAGS));
            if (binding.keysym == XKB_KEY_NoSymbol)
                fail("unknown key '" + key + "'");
            binding.action = parse_action(field(L, "action"));
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
            if (config.binding(binding.modifiers, binding.keysym))
                fail("duplicate keyboard binding");
            config.bindings.push_back(std::move(binding));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
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
    };
    for (const auto &[candidate, action] : actions)
        if (name == candidate)
            return action;
    fail("unknown action '" + name + "'");
}

bool action_takes_workspace(sh_action action) {
    return action == SH_WORKSPACE || action == SH_MOVE_TO_WORKSPACE;
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

Config parse_config(const std::string &source, const std::string &name) {
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
    *static_cast<int *>(lua_getextraspace(L)) = 1000;
    lua_sethook(L, instruction_limit, LUA_MASKCOUNT, 1000);
    if (luaL_loadbufferx(L, source.data(), source.size(), name.c_str(), "t") != LUA_OK ||
        lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        fail(message ? message : "Lua raised a non-string error");
    }
    return read(L);
}

Config load_config(const std::filesystem::path &path) {
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
    return parse_config(source, "@" + path.string());
}
} // namespace shaode
