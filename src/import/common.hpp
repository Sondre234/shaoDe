// SPDX-License-Identifier: GPL-3.0-or-later
// Internals shared by the dotfile importer's readers; see docs/dotfile-import.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shaode::import {
namespace fs = std::filesystem;

struct Origin {
    fs::path file;
    int line = 0;
};

// Every file the importer reads comes through here. Reads stay inside the source directory
// and, when that is a ~/.config, the ~/.cache beside it (wallbash and pywal write there).
// Absolute ~/.config/... references inside a copied dotfiles directory land in that copy.
class Files {
  public:
    explicit Files(const fs::path &source);
    const fs::path &source() const { return source_; }
    // Absolute, ~/..., or relative to `base`; nothing outside the allowed roots.
    std::optional<fs::path> resolve(std::string_view path, const fs::path &base) const;
    // At most 1 MiB; nothing when the file is missing or unreadable.
    std::optional<std::string> read(const fs::path &path) const;
    // The first of `candidates` (relative to the source directory) that exists.
    std::optional<fs::path> find(std::initializer_list<const char *> candidates) const;
    // A file in the cache directory, if there is one and the file exists.
    std::optional<fs::path> cache(const char *relative) const;
    // The same file's path in the cache, without following links.
    std::optional<fs::path> cache_path(const char *relative) const;
    // "~/.config/hypr/hyprland.lua:12" for reports.
    std::string show(const Origin &origin) const;

  private:
    bool allowed(const fs::path &path) const;
    fs::path source_, cache_, home_;
};

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};
// "#rrggbb", or "#rrggbbaa" when translucent and `alpha` is set.
std::string hex(Color color, bool alpha = true);
std::optional<Color> parse_hex(std::string_view digits); // "rrggbb", "rgb", "rrggbbaa", ...

std::string trim(std::string_view text);
// Splits on `separator` outside parentheses and quotes, trimming each part.
std::vector<std::string> split(std::string_view text, char separator);
std::optional<double> parse_number(std::string_view text);
std::optional<bool> parse_bool(std::string_view text);

// Everything the importer could not carry over, for the report.
struct Report {
    std::vector<std::string> skipped;   // "where: what (why)"
    std::map<std::string, int> ignored; // "key bindings" -> how many
    void skip(const Files &files, const Origin &origin, const std::string &message);
};

// A parsed JSON value (Waybar, pywal); JSONC comments and trailing commas are accepted.
struct Json {
    struct Field;
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::vector<Field> fields;
    int line = 0;
    const Json *get(std::string_view key) const;
};
struct Json::Field {
    std::string key;
    Json value;
};
// Throws std::runtime_error with a line number on malformed input.
Json parse_json(std::string_view text);

// Hyprland settings from hyprland.lua or hyprland.conf, before translation.
struct HyprValue {
    std::string text;
    Origin origin;
};
// A monitor or window rule: "output", "mode", ... or "match:class", "opacity", ...
struct HyprItem {
    std::map<std::string, std::string> fields;
    Origin origin;
    bool enabled = true;
};
struct Hyprland {
    fs::path file;
    std::map<std::string, HyprValue> options; // "general:gaps_in", "general:col.active_border"
    std::vector<HyprItem> monitors, rules;
};
std::optional<Hyprland> read_hyprland(const Files &files, Report &report);

// CSS as GTK and Waybar use it: @import, @define-color, and flat rules.
struct Css {
    struct Declaration {
        std::string selector, property, value;
        Origin origin;
    };
    std::map<std::string, HyprValue> colors; // @define-color name -> value
    std::vector<Declaration> declarations;   // in cascade order
};
void parse_css(const Files &files, const fs::path &path, Css &css, Report &report, int depth = 0);
std::optional<Color> css_color(std::string_view value, const Css &css, int depth = 0);

// Waybar's bar look, already in shaoDe terms: "position", "height", "margin-top", ...,
// "background", "color", "radius", "font", "font_size".
std::map<std::string, HyprValue> read_waybar(const Files &files, Report &report);
} // namespace shaode::import
