// SPDX-License-Identifier: GPL-3.0-or-later
// Reads Hyprland's configuration: hyprland.lua in a sandbox, or hyprland.conf (hyprlang).
#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fnmatch.h>
#include <lua.hpp>

namespace shaode::import {
namespace {
// hyprland.lua is code. It runs with hl.* functions that only record their arguments, no way
// to write files or start processes, reads limited to the imported directory, and a budget.
constexpr size_t memory_limit = 64u << 20;
constexpr int budget = 10000; // hook calls, 10000 instructions apart

struct Sandbox {
    const Files *files;
    fs::path directory; // hyprland.lua's, for relative paths
    size_t memory = 0;
    int remaining = budget;
};
Sandbox &sandbox(lua_State *L) {
    void *data = nullptr;
    lua_getallocf(L, &data);
    return *static_cast<Sandbox *>(data);
}
void *allocate(void *data, void *block, size_t old, size_t size) {
    auto &self = *static_cast<Sandbox *>(data);
    if (!block)
        old = 0;
    if (size == 0) {
        std::free(block);
        self.memory -= old;
        return nullptr;
    }
    if (size > old && self.memory + (size - old) > memory_limit)
        return nullptr;
    void *result = std::realloc(block, size);
    if (result)
        self.memory = self.memory - old + size;
    return result;
}
void count(lua_State *L, lua_Debug *) {
    if (--sandbox(L).remaining <= 0)
        luaL_error(L, "hyprland.lua ran too long");
}
// Replaces argument 1 with its resolved path; false (with nil, message pushed) when the file
// is outside the imported directory.
bool redirect(lua_State *L) {
    auto &self = sandbox(L);
    const char *name = luaL_checkstring(L, 1);
    std::string path;
    if (auto resolved = self.files->resolve(name, self.directory))
        path = resolved->string();
    if (path.empty()) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s: outside the imported directory", name);
        return false;
    }
    lua_pushlstring(L, path.data(), path.size());
    lua_replace(L, 1);
    return true;
}
// Chunk names relative to the imported directory, so Lua does not truncate them in messages.
std::string chunk_name(const Files &files, const fs::path &path) {
    auto relative = path.lexically_relative(files.source());
    return "@" + (relative.empty() || *relative.begin() == ".." ? path : relative).string();
}
fs::path chunk_path(const Files &files, const std::string &source) {
    fs::path path = source.starts_with("@") ? source.substr(1) : source;
    return path.is_relative() ? files.source() / path : path;
}
// io.open and io.lines, read-only and redirected; the originals are upvalue 1.
int open_file(lua_State *L) {
    lua_settop(L, 2);
    const char *mode = luaL_optstring(L, 2, "r");
    if (std::strpbrk(mode, "wa+")) {
        lua_pushnil(L);
        lua_pushliteral(L, "files are read-only while importing");
        return 2;
    }
    if (!redirect(L))
        return 2;
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, 2, LUA_MULTRET);
    return lua_gettop(L);
}
int lines(lua_State *L) {
    if (!redirect(L))
        return lua_error(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
    return lua_gettop(L);
}
// loadfile(name [, mode [, env]]): text chunks only.
int load_file(lua_State *L) {
    lua_settop(L, 3);
    if (!redirect(L))
        return 2;
    auto &self = sandbox(L);
    std::string text;
    if (auto contents = self.files->read(lua_tostring(L, 1)))
        text = std::move(*contents);
    else {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot read %s", lua_tostring(L, 1));
        return 2;
    }
    std::string chunk = chunk_name(*self.files, lua_tostring(L, 1));
    int status = luaL_loadbufferx(L, text.data(), text.size(), chunk.c_str(), "t");
    std::string().swap(text);
    if (status != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    if (!lua_isnil(L, 3)) {
        lua_pushvalue(L, 3);
        if (!lua_setupvalue(L, -2, 1))
            lua_pop(L, 1);
    }
    return 1;
}
int do_file(lua_State *L) {
    lua_settop(L, 1);
    if (load_file(L) != 1)
        return lua_error(L);
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - 1;
}
int nothing(lua_State *) { return 0; }

// Records hl.config/monitor/window_rule/device calls; everything else in hl is a stub that
// accepts any use. Unknown globals (HyDE's `hyde`, for one) are stubs too.
constexpr const char *prelude = R"lua(
local records, ignored, unknown, getinfo, raw_load = ...
local stub_meta = {}
local stub = setmetatable({}, stub_meta)
stub_meta.__index = function() return stub end
stub_meta.__call = function() return stub end
stub_meta.__concat = function(a, b)
    return (type(a) == "string" and a or "") .. (type(b) == "string" and b or "")
end
stub_meta.__tostring = function() return "" end
local function record(kind)
    return function(data)
        local info = getinfo(2, "Sl")
        local entry = { kind = kind, data = data, source = info.source,
                        line = info.currentline, enabled = true }
        records[#records + 1] = entry
        return setmetatable({ set_enabled = function(_, on) entry.enabled = on and true or false end },
                            stub_meta)
    end
end
local function ignore(what)
    return function()
        if what then ignored[what] = (ignored[what] or 0) + 1 end
        return stub
    end
end
hl = setmetatable({
    config = record("config"), monitor = record("monitor"),
    window_rule = record("window_rule"), device = record("device"),
    bind = ignore("key bindings"), unbind = ignore(), on = ignore("event handlers"),
    timer = ignore("timers"), exec_cmd = ignore("commands"),
    env = ignore("environment variables"), curve = ignore("animation curves"),
    animation = ignore("animations"), gesture = ignore("gestures"),
    workspace_rule = ignore("workspace rules"), layer_rule = ignore("layer rules"),
    get_active_window = function() return nil end,
    get_config = function() return nil end,
    is_key_down = function() return false end,
}, { __index = function() return stub end })
function load(chunk, name, _, env) return raw_load(chunk, name, "t", env) end
local modules = {}
function require(name)
    if modules[name] == nil then
        local chunk, message = loadfile((tostring(name):gsub("%.", "/")) .. ".lua")
        if not chunk then error("module '" .. tostring(name) .. "' not found: " .. message, 2) end
        local result = chunk(name)
        modules[name] = result == nil and true or result
    end
    return modules[name]
end
setmetatable(_G, { __index = function(_, name)
    unknown[#unknown + 1] = tostring(name)
    return stub
end })
)lua";

std::string scalar(lua_State *L, int index) {
    switch (lua_type(L, index)) {
    case LUA_TBOOLEAN:
        return lua_toboolean(L, index) ? "true" : "false";
    case LUA_TNUMBER: {
        if (lua_isinteger(L, index))
            return std::to_string(lua_tointeger(L, index));
        char text[32];
        auto end = std::to_chars(text, text + sizeof(text), lua_tonumber(L, index)).ptr;
        return {text, end};
    }
    case LUA_TSTRING: {
        size_t length = 0;
        const char *text = lua_tolstring(L, index, &length);
        return {text, length};
    }
    default:
        return {};
    }
}
// hl.config's nested tables become hyprlang names: general.gaps_in -> "general:gaps_in",
// general.col.active_border -> "general:col.active_border". A gradient { colors, angle }
// becomes hyprlang's "rgba(...) rgba(...) 45deg"; other lists join with spaces.
void flatten(lua_State *L, int index, const std::string &prefix, bool colors,
             std::map<std::string, std::string> &out, int depth = 0) {
    if (depth > 8)
        return;
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        std::string key = lua_type(L, -2) == LUA_TSTRING ? scalar(L, -2)
                          : lua_isinteger(L, -2)         ? std::to_string(lua_tointeger(L, -2))
                                                         : std::string();
        auto name = prefix.empty() ? key : prefix + (colors ? "." : ":") + key;
        if (key.empty()) {
        } else if (!lua_istable(L, -1)) {
            if (auto text = scalar(L, -1); !text.empty() || lua_type(L, -1) == LUA_TSTRING)
                out[name] = text;
        } else if (lua_getfield(L, -1, "colors") == LUA_TTABLE) {
            std::string text;
            for (lua_Integer i = 1; i <= static_cast<lua_Integer>(lua_rawlen(L, -1)); ++i) {
                lua_rawgeti(L, -1, i);
                text += (text.empty() ? "" : " ") + scalar(L, -1);
                lua_pop(L, 1);
            }
            if (lua_getfield(L, -2, "angle") == LUA_TNUMBER)
                text += " " + scalar(L, -1) + "deg";
            lua_pop(L, 2);
            out[name] = text;
        } else if (lua_pop(L, 1); lua_rawlen(L, -1) > 0) {
            std::string text;
            for (lua_Integer i = 1; i <= static_cast<lua_Integer>(lua_rawlen(L, -1)); ++i) {
                lua_rawgeti(L, -1, i);
                text += (text.empty() ? "" : " ") + scalar(L, -1);
                lua_pop(L, 1);
            }
            out[name] = text;
        } else {
            flatten(L, -1, name, key == "col", out, depth + 1);
        }
        lua_pop(L, 1);
    }
}

void read_lua(const Files &files, const fs::path &path, Hyprland &hypr, Report &report) {
    auto text = files.read(path);
    Origin top{path, 0};
    if (!text) {
        report.skip(files, top, "cannot read");
        return;
    }
    Sandbox box{&files, path.parent_path()};
    std::unique_ptr<lua_State, decltype(&lua_close)> state(lua_newstate(allocate, &box), lua_close);
    if (!state)
        throw std::runtime_error("cannot allocate a Lua state");
    auto *L = state.get();
    for (auto [name, open] : {std::pair{"_G", luaopen_base},
                              {LUA_TABLIBNAME, luaopen_table},
                              {LUA_STRLIBNAME, luaopen_string},
                              {LUA_MATHLIBNAME, luaopen_math},
                              {LUA_UTF8LIBNAME, luaopen_utf8}}) {
        luaL_requiref(L, name, open, 1);
        lua_pop(L, 1);
    }
    for (const char *name : {"collectgarbage", "dofile", "loadfile"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    lua_pushcfunction(L, nothing);
    lua_setglobal(L, "print");
    lua_pushcfunction(L, load_file);
    lua_setglobal(L, "loadfile");
    lua_pushcfunction(L, do_file);
    lua_setglobal(L, "dofile");
    luaL_requiref(L, LUA_IOLIBNAME, luaopen_io, 0);
    lua_newtable(L);
    for (auto [name, wrapper] : {std::pair{"open", open_file}, {"lines", lines}}) {
        lua_getfield(L, -2, name);
        lua_pushcclosure(L, wrapper, 1);
        lua_setfield(L, -2, name);
    }
    lua_getfield(L, -2, "type");
    lua_setfield(L, -2, "type");
    lua_setglobal(L, "io");
    lua_pop(L, 1);
    luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 0);
    lua_newtable(L);
    for (const char *name : {"getenv", "time", "date", "clock", "difftime"}) {
        lua_getfield(L, -2, name);
        lua_setfield(L, -2, name);
    }
    lua_setglobal(L, "os");
    lua_pop(L, 1);

    int tables[3];
    if (luaL_loadbufferx(L, prelude, std::strlen(prelude), "=prelude", "t") != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));
    for (int &ref : tables) {
        lua_newtable(L);
        lua_pushvalue(L, -1);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    luaL_requiref(L, LUA_DBLIBNAME, luaopen_debug, 0);
    lua_getfield(L, -1, "getinfo");
    lua_remove(L, -2);
    lua_getglobal(L, "load");
    if (lua_pcall(L, 5, 0, 0) != LUA_OK)
        throw std::runtime_error(lua_tostring(L, -1));

    lua_sethook(L, count, LUA_MASKCOUNT, 10000);
    auto chunk = chunk_name(files, path);
    if (luaL_loadbufferx(L, text->data(), text->size(), chunk.c_str(), "t") != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        report.skip(files, top,
                    std::string("stopped with an error; settings made before it still count: ") +
                        (message ? message : "unknown error"));
        lua_settop(L, 0);
    }
    lua_sethook(L, nullptr, 0, 0);

    lua_rawgeti(L, LUA_REGISTRYINDEX, tables[0]);
    for (lua_Integer i = 1; i <= static_cast<lua_Integer>(lua_rawlen(L, -1)); ++i) {
        lua_rawgeti(L, -1, i);
        lua_getfield(L, -1, "kind");
        auto kind = scalar(L, -1);
        lua_getfield(L, -2, "source");
        std::string source = scalar(L, -1);
        lua_getfield(L, -3, "line");
        Origin origin{chunk_path(files, source), static_cast<int>(lua_tointeger(L, -1))};
        lua_getfield(L, -4, "enabled");
        bool enabled = lua_toboolean(L, -1);
        lua_getfield(L, -5, "data");
        std::map<std::string, std::string> fields;
        if (lua_istable(L, -1))
            flatten(L, -1, "", false, fields);
        else
            report.skip(files, origin, "hl." + kind + " without a table");
        lua_pop(L, 6);
        if (kind == "config") {
            for (auto &[name, value] : fields)
                hypr.options[name] = {value, origin};
        } else if (kind == "monitor") {
            hypr.monitors.push_back({std::move(fields), origin, enabled});
        } else if (kind == "window_rule") {
            hypr.rules.push_back({std::move(fields), origin, enabled});
        } else if (kind == "device") {
            report.skip(files, origin,
                        "per-device input settings for '" + fields["name"] + "' are not imported");
        }
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, tables[1]);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        report.ignored[scalar(L, -2)] += static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, tables[2]);
    std::vector<std::string> unknown;
    for (lua_Integer i = 1; i <= static_cast<lua_Integer>(lua_rawlen(L, -1)); ++i) {
        lua_rawgeti(L, -1, i);
        if (auto name = scalar(L, -1);
            std::find(unknown.begin(), unknown.end(), name) == unknown.end())
            unknown.push_back(name);
        lua_pop(L, 1);
    }
    for (const auto &name : unknown)
        report.skip(files, top,
                    "uses '" + name +
                        "', which exists only when Hyprland runs it; treated as a stub");
}

// hyprlang: `source`, `$variables`, `section { }` blocks, `a:b = c` names, and the keywords
// that carry monitors and window rules.
class Hyprlang {
  public:
    Hyprlang(const Files &files, Hyprland &hypr, Report &report)
        : files_(files), hypr_(hypr), report_(report) {}

    void read(const fs::path &path, int depth = 0) {
        if (depth > 16 || ++count_ > 256) {
            report_.skip(files_, {path, 0}, "too many nested `source` files");
            return;
        }
        auto text = files_.read(path);
        if (!text) {
            report_.skip(files_, {path, 0}, "cannot read");
            return;
        }
        std::vector<std::string> sections;
        HyprItem block;
        std::string block_kind;
        int number = 0;
        size_t start = 0;
        while (start <= text->size()) {
            auto end = text->find('\n', start);
            if (end == std::string::npos)
                end = text->size();
            auto line = trim(uncomment(std::string_view(*text).substr(start, end - start)));
            start = end + 1;
            Origin origin{path, ++number};
            if (line.empty())
                continue;
            if (line.back() == '{') {
                auto name = trim(line.substr(0, line.size() - 1));
                if (sections.empty() && (name == "monitorv2" || name == "windowrule" ||
                                         name == "windowrulev2" || name.starts_with("device"))) {
                    block_kind = name.starts_with("device") ? "device" : name;
                    block = {{}, origin, true};
                }
                sections.push_back(name);
                continue;
            }
            if (line.front() == '}') {
                if (sections.empty()) {
                    report_.skip(files_, origin, "unmatched '}'");
                    continue;
                }
                sections.pop_back();
                if (sections.empty() && !block_kind.empty()) {
                    finish_block(block_kind, std::move(block));
                    block_kind.clear();
                }
                continue;
            }
            auto equals = line.find('=');
            if (equals == std::string::npos) {
                report_.skip(files_, origin, "not a hyprlang assignment: " + line);
                continue;
            }
            auto key = trim(std::string_view(line).substr(0, equals));
            auto value = trim(std::string_view(line).substr(equals + 1));
            if (key.starts_with("$")) {
                variables_[key.substr(1)] = expand(value);
                continue;
            }
            value = expand(value);
            if (!block_kind.empty()) {
                block.fields[key] = value;
                continue;
            }
            std::string name;
            for (const auto &section : sections)
                name += section + ":";
            name += key;
            if (sections.empty() && keyword(key, value, origin, path.parent_path(), depth))
                continue;
            hypr_.options[name] = {value, origin};
        }
    }

  private:
    static std::string uncomment(std::string_view line) {
        std::string result;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] != '#') {
                result += line[i];
            } else if (i + 1 < line.size() && line[i + 1] == '#') {
                result += '#';
                ++i;
            } else {
                break;
            }
        }
        return result;
    }
    std::string expand(std::string_view value) const {
        std::string result;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] != '$') {
                result += value[i];
                continue;
            }
            size_t end = i + 1;
            while (end < value.size() &&
                   (std::isalnum(static_cast<unsigned char>(value[end])) || value[end] == '_'))
                ++end;
            // The longest defined name wins, as in hyprlang.
            bool found = false;
            for (size_t length = end - i - 1; length > 0 && !found; --length)
                if (auto it = variables_.find(std::string(value.substr(i + 1, length)));
                    it != variables_.end()) {
                    result += it->second;
                    i += length;
                    found = true;
                }
            if (!found)
                result += '$';
        }
        return result;
    }
    void finish_block(const std::string &kind, HyprItem block) {
        if (kind == "monitorv2")
            hypr_.monitors.push_back(std::move(block));
        else if (kind == "device")
            report_.skip(files_, block.origin,
                         "per-device input settings for '" + block.fields["name"] +
                             "' are not imported");
        else
            hypr_.rules.push_back(std::move(block));
    }
    // Keywords that are not plain options; true when handled.
    bool keyword(const std::string &key, const std::string &value, const Origin &origin,
                 const fs::path &directory, int depth) {
        if (key == "source") {
            source(value, origin, directory, depth);
        } else if (key == "monitor") {
            monitor(value, origin);
        } else if (key == "windowrule" || key == "windowrulev2") {
            hypr_.rules.push_back(rule(key, value, origin));
        } else if (key.starts_with("bind")) {
            ++report_.ignored["key bindings"];
        } else if (key.starts_with("exec")) {
            ++report_.ignored["commands"];
        } else {
            static const std::pair<const char *, const char *> ignored[] = {
                {"env", "environment variables"},
                {"animation", "animations"},
                {"bezier", "animation curves"},
                {"layerrule", "layer rules"},
                {"workspace", "workspace rules"},
                {"gesture", "gestures"},
                {"plugin", "plugins"},
                {"permission", "permissions"},
                {"unbind", nullptr}};
            for (auto [name, label] : ignored)
                if (key == name) {
                    if (label)
                        ++report_.ignored[label];
                    return true;
                }
            return false;
        }
        return true;
    }
    void source(const std::string &value, const Origin &origin, const fs::path &directory,
                int depth) {
        fs::path pattern(value);
        if (value.find_first_of("*?[") == std::string::npos) {
            if (auto path = files_.resolve(value, directory))
                read(*path, depth + 1);
            else
                report_.skip(files_, origin,
                             "source " + value + ": missing or outside the directory");
            return;
        }
        auto parent = files_.resolve(pattern.parent_path().string(), directory);
        if (!parent || !fs::is_directory(*parent)) {
            report_.skip(files_, origin, "source " + value + ": missing or outside the directory");
            return;
        }
        std::vector<fs::path> matches;
        for (const auto &entry : fs::directory_iterator(*parent))
            if (fnmatch(pattern.filename().c_str(), entry.path().filename().c_str(), 0) == 0)
                matches.push_back(entry.path());
        std::sort(matches.begin(), matches.end());
        for (const auto &match : matches)
            if (auto path = files_.resolve(match.string(), directory))
                read(*path, depth + 1);
    }
    // monitor = NAME, MODE, POSITION, SCALE[, transform, N][, vrr, N]... or NAME, disable
    void monitor(const std::string &value, const Origin &origin) {
        auto parts = split(value, ',');
        HyprItem item{{}, origin, true};
        item.fields["output"] = parts.empty() ? "" : parts[0];
        if (parts.size() >= 2 && (parts[1] == "disable" || parts[1] == "disabled")) {
            item.fields["disabled"] = "true";
        } else if (parts.size() >= 2 && parts[1] == "addreserved") {
            report_.skip(files_, origin, "reserved monitor areas are not imported");
            return;
        } else {
            const char *names[] = {"mode", "position", "scale"};
            for (size_t i = 1; i < parts.size() && i < 4; ++i)
                item.fields[names[i - 1]] = parts[i];
            for (size_t i = 4; i + 1 < parts.size(); i += 2)
                item.fields[parts[i]] = parts[i + 1];
        }
        hypr_.monitors.push_back(std::move(item));
    }
    // windowrule = EFFECT ARGS, class:REGEX (v1/v2) or match:class REGEX, EFFECT ARGS (v3).
    static HyprItem rule(const std::string &keyword, const std::string &value,
                         const Origin &origin) {
        HyprItem item{{}, origin, true};
        auto parts = split(value, ',');
        for (size_t i = 0; i < parts.size(); ++i) {
            const auto &part = parts[i];
            auto colon = part.find(':');
            auto space = part.find_first_of(" \t");
            bool property =
                colon != std::string::npos && colon < space &&
                std::all_of(
                    part.begin(), part.begin() + static_cast<long>(colon),
                    [](char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; });
            if (part.starts_with("match:")) {
                auto key = part.substr(0, std::min(space, part.size()));
                item.fields[key] = space == std::string::npos ? "" : trim(part.substr(space));
            } else if (property && i > 0) {
                auto name = part.substr(0, colon);
                if (name == "initialClass")
                    name = "initial_class";
                else if (name == "initialTitle")
                    name = "initial_title";
                item.fields["match:" + name] = trim(part.substr(colon + 1));
            } else if (i > 0 && keyword == "windowrule" && parts.size() == 2 &&
                       !parts[0].starts_with("match:")) { // windowrule = EFFECT, CLASS-REGEX
                item.fields["match:class"] = part;
            } else {
                auto name = part.substr(0, std::min(space, part.size()));
                item.fields[name] = space == std::string::npos ? "" : trim(part.substr(space));
            }
        }
        return item;
    }

    const Files &files_;
    Hyprland &hypr_;
    Report &report_;
    std::map<std::string, std::string> variables_;
    int count_ = 0;
};
} // namespace

std::optional<Hyprland> read_hyprland(const Files &files, Report &report) {
    auto lua = files.find({"hypr/hyprland.lua", "hyprland.lua"});
    auto conf = files.find({"hypr/hyprland.conf", "hyprland.conf"});
    Hyprland hypr;
    if (lua) {
        hypr.file = *lua;
        if (conf)
            report.skip(files, {*conf, 0}, "not read: Hyprland uses hyprland.lua instead");
        read_lua(files, *lua, hypr, report);
    } else if (conf) {
        hypr.file = *conf;
        Hyprlang(files, hypr, report).read(*conf);
    } else {
        return std::nullopt;
    }
    return hypr;
}
} // namespace shaode::import
