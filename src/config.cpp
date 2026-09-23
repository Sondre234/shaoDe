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
void text_field(lua_State *L, const char *key, char (&target)[128]) {
    lua_getfield(L, -1, key);
    if (!lua_isnil(L, -1)) {
        auto value = string(L, -1, key);
        if (value.size() >= sizeof(target))
            fail(std::string(key) + " is too long");
        std::memcpy(target, value.c_str(), value.size() + 1);
    }
    lua_pop(L, 1);
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
    if (name == "Alt")
        return SH_ALT;
    if (name == "Super")
        return SH_LOGO;
    if (name == "Ctrl")
        return SH_CTRL;
    if (name == "Shift")
        return SH_SHIFT;
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
sh_action action(const std::string &name) {
    if (name == "spawn")
        return SH_HANDLED;
    if (name == "quit")
        return SH_QUIT;
    if (name == "close")
        return SH_CLOSE;
    if (name == "cycle")
        return SH_CYCLE;
    if (name == "snap_left")
        return SH_SNAP_LEFT;
    if (name == "snap_right")
        return SH_SNAP_RIGHT;
    if (name == "maximize")
        return SH_MAXIMIZE;
    if (name == "restore")
        return SH_RESTORE;
    if (name == "tile")
        return SH_TILE;
    if (name == "reload")
        return SH_RELOAD;
    fail("unknown action '" + name + "'");
}
void instruction_limit(lua_State *L, lua_Debug *) {
    auto *remaining = static_cast<int *>(lua_getextraspace(L));
    if (--*remaining <= 0)
        luaL_error(L, "configuration exceeded its instruction budget");
}
Config read(lua_State *L) {
    Config config;
    table(L, -1, "configuration result");
    keys(L, -1, {"version", "appearance", "keyboard", "mouse", "layout", "bindings", "startup"});
    if (integer(L, "version", 1, 1, 1) != 1)
        fail("unsupported version");
    lua_getfield(L, -1, "appearance");
    if (!lua_isnil(L, -1)) {
        table(L, -1, "appearance");
        keys(L, -1, {"background"});
        auto color = field(L, "background");
        if (color.size() != 7 || color[0] != '#' ||
            color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
            fail("background must be #RRGGBB");
        for (size_t i = 0; i < 3; ++i)
            config.settings.background[i] =
                std::stoi(color.substr(1 + 2 * i, 2), nullptr, 16) / 255.0F;
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "keyboard");
    if (!lua_isnil(L, -1)) {
        table(L, -1, "keyboard");
        keys(L, -1, {"layout", "options", "repeat_rate", "repeat_delay"});
        text_field(L, "layout", config.settings.keyboard_layout);
        text_field(L, "options", config.settings.keyboard_options);
        config.settings.repeat_rate = integer(L, "repeat_rate", 25, 0, 100);
        config.settings.repeat_delay = integer(L, "repeat_delay", 600, 0, 5000);
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "mouse");
    if (!lua_isnil(L, -1)) {
        table(L, -1, "mouse");
        keys(L, -1, {"modifier"});
        config.settings.mouse_modifier = modifier(field(L, "modifier"));
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "layout");
    if (!lua_isnil(L, -1)) {
        table(L, -1, "layout");
        keys(L, -1, {"gap"});
        config.settings.gap = integer(L, "gap", 8, 0, 100);
    }
    lua_pop(L, 1);
    lua_getfield(L, -1, "bindings");
    if (!lua_isnil(L, -1)) {
        auto size = array_size(L, -1, 512);
        for (size_t i = 1; i <= size; ++i) {
            lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
            table(L, -1, "binding");
            keys(L, -1, {"mods", "key", "action", "command"});
            Binding binding{};
            auto key = field(L, "key");
            binding.keysym =
                xkb_keysym_to_lower(xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_NO_FLAGS));
            if (binding.keysym == XKB_KEY_NoSymbol)
                fail("unknown key '" + key + "'");
            binding.action = action(field(L, "action"));
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
