// SPDX-License-Identifier: GPL-3.0-or-later
// Translates what the readers found into shaoDe settings and writes theme.lua.
#include "shaode/import.hpp"

#include "common.hpp"
#include "shaode/config.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <regex>
#include <set>
#include <stdexcept>

namespace shaode {
namespace {
using namespace import;

std::string quote(std::string_view text) {
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\')
            result += {'\\', static_cast<char>(c)};
        else if (c == '\n')
            result += "\\n";
        else if (c < 0x20 || c == 0x7f) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\%03u", c);
            result += escaped;
        } else
            result += static_cast<char>(c);
    }
    return result + "\"";
}
std::string number(double value) {
    char text[32];
    auto end = std::to_chars(text, text + sizeof(text), value).ptr;
    std::string result(text, end);
    if (result.find_first_of(".e") == std::string::npos && std::abs(value) < 1e15)
        result += ".0"; // keep it a float: shaoDe checks integers strictly
    return result;
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), ::tolower);
    return text;
}

struct Entry {
    std::vector<std::string> path;
    std::string lua;
    Origin origin;
    std::string note;
};
// Settings to write, in the order they were decided; a later source replaces an earlier one.
class Theme {
  public:
    void set(std::vector<std::string> path, std::string lua, Origin origin, std::string note = {}) {
        erase(path);
        entries_.push_back({std::move(path), std::move(lua), std::move(origin), std::move(note)});
    }
    void erase(const std::vector<std::string> &prefix) {
        std::erase_if(entries_, [&](const Entry &entry) {
            return entry.path.size() >= prefix.size() &&
                   std::equal(prefix.begin(), prefix.end(), entry.path.begin());
        });
    }
    bool has(const std::vector<std::string> &path) const {
        return std::any_of(entries_.begin(), entries_.end(),
                           [&](const Entry &entry) { return entry.path == path; });
    }
    const std::vector<Entry> &entries() const { return entries_; }

  private:
    std::vector<Entry> entries_;
};

std::optional<Color> hypr_color(std::string_view token) {
    auto text = lower(trim(token));
    auto inner = [&](const char *name) -> std::optional<std::vector<std::string>> {
        auto prefix = std::string(name) + "(";
        if (!text.starts_with(prefix) || !text.ends_with(")"))
            return std::nullopt;
        return split(std::string_view(text).substr(prefix.size(), text.size() - prefix.size() - 1),
                     ',');
    };
    for (auto [name, size] : {std::pair{"rgba", 4u}, {"rgb", 3u}})
        if (auto parts = inner(name)) {
            if (parts->size() == 1) {
                auto digits = (*parts)[0];
                return digits.size() == 2 * size ? parse_hex(digits) : std::nullopt;
            }
            if (parts->size() != size)
                return std::nullopt;
            double values[4] = {0, 0, 0, 1};
            for (size_t i = 0; i < size; ++i) {
                auto value = parse_number((*parts)[i]);
                if (!value)
                    return std::nullopt;
                values[i] = *value;
            }
            auto byte = [](double v) {
                return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 255.0)));
            };
            return Color{byte(values[0]), byte(values[1]), byte(values[2]), byte(values[3] * 255)};
        }
    // 0xAARRGGBB, or the same number in decimal from hyprland.lua.
    uint64_t argb = 0;
    const char *begin = text.data(), *end = text.data() + text.size();
    int base = 10;
    if (text.starts_with("0x")) {
        begin += 2;
        base = 16;
    }
    auto [stop, error] = std::from_chars(begin, end, argb, base);
    if (error != std::errc{} || stop != end || begin == end || argb > 0xffffffff)
        return std::nullopt;
    return Color{static_cast<uint8_t>(argb >> 16), static_cast<uint8_t>(argb >> 8),
                 static_cast<uint8_t>(argb), static_cast<uint8_t>(argb >> 24)};
}
// The first color of "rgba(...) rgba(...) 45deg"; `gradient` says whether there were more.
std::optional<Color> first_color(std::string_view text, bool &gradient) {
    std::optional<Color> result;
    int colors = 0;
    for (const auto &token : split(text, ' '))
        if (auto color = hypr_color(token)) {
            if (!result)
                result = color;
            ++colors;
        }
    gradient = colors > 1;
    return result;
}

class Translator {
  public:
    Translator(const Files &files, Theme &theme, Report &report)
        : files_(files), theme_(theme), report_(report) {}

    void palette() {
        struct Swatch {
            Color color;
            Origin origin;
        };
        std::optional<Swatch> background, foreground, accent;
        std::string name;
        if (auto path = files_.cache("hyde/wallbash/gtk.css")) {
            name = "wallbash";
            Css css;
            parse_css(files_, *path, css, report_);
            auto swatch = [&](const char *key) -> std::optional<Swatch> {
                auto it = css.colors.find(key);
                if (it == css.colors.end())
                    return std::nullopt;
                auto color = css_color(it->second.text, css);
                return color ? std::optional(Swatch{*color, it->second.origin}) : std::nullopt;
            };
            background = swatch("wallbash_pry1");
            foreground = swatch("wallbash_txt1");
            accent = swatch("wallbash_1xa6");
        } else if (auto path = files_.cache("wal/colors.json") ? files_.cache("wal/colors.json")
                                                               : files_.find({"wal/colors.json"})) {
            name = "pywal";
            try {
                auto text = files_.read(*path);
                auto json = parse_json(text ? *text : "");
                auto swatch = [&](const char *group, const char *key) -> std::optional<Swatch> {
                    auto *table = json.get(group);
                    auto *value = table ? table->get(key) : nullptr;
                    auto color =
                        value && value->kind == Json::String && value->text.starts_with("#")
                            ? parse_hex(std::string_view(value->text).substr(1))
                            : std::nullopt;
                    return color ? std::optional(Swatch{*color, {*path, value->line}})
                                 : std::nullopt;
                };
                background = swatch("special", "background");
                foreground = swatch("special", "foreground");
                accent = swatch("colors", "color4");
            } catch (const std::exception &error) {
                report_.skip(files_, {*path, 0}, std::string("not valid JSON: ") + error.what());
            }
        }
        auto note = name + " palette";
        if (background) {
            auto color = background->color;
            color.a = 255;
            theme_.set({"appearance", "background"}, quote(hex(color, false)), background->origin,
                       note);
            theme_.set({"shell", "panel_color"}, quote(hex(background->color)), background->origin,
                       note);
        }
        if (foreground)
            theme_.set({"shell", "text_color"}, quote(hex(foreground->color)), foreground->origin,
                       note);
        if (accent)
            theme_.set({"shell", "accent"}, quote(hex(accent->color, false)), accent->origin, note);
    }

    void hyprland(const Hyprland &hypr) {
        hypr_ = &hypr;
        whole("general:gaps_in", {"layout", "gap_inner"}, 0, 100, 2,
              "doubled: Hyprland puts gaps_in on each side of a window");
        whole("general:gaps_out", {"layout", "gap_outer"}, 0, 100);
        whole("general:border_size", {"windows", "border_width"}, 0, 20);
        for (auto [name, key] : {std::pair{"general:col.active_border", "border_color"},
                                 {"general:col.inactive_border", "border_inactive_color"}})
            if (auto *value = option(name)) {
                bool gradient = false;
                auto color = first_color(value->text, gradient);
                if (!color) {
                    report_.skip(files_, value->origin,
                                 std::string(name) + ": cannot read the color " + value->text);
                    continue;
                }
                auto note = gradient ? "first color of a gradient" : "";
                theme_.set({"windows", key}, quote(hex(*color)), value->origin, note);
                if (key == std::string("border_color")) {
                    color->a = 255;
                    theme_.set({"shell", "accent"}, quote(hex(*color, false)), value->origin,
                               "the active border color");
                }
            }
        fraction("decoration:active_opacity", {"windows", "opacity"}, 0.05, 1);
        fraction("decoration:inactive_opacity", {"windows", "inactive_opacity"}, 0.05, 1);
        if (auto *rounding = option("decoration:rounding");
            rounding && parse_number(rounding->text).value_or(0) > 0)
            report_.skip(files_, rounding->origin,
                         "rounded corners need a renderer shaoDe lacks so far");
        for (const char *effect : {"blur", "shadow"}) {
            auto prefix = std::string("decoration:") + effect + ":";
            if (auto *enabled = option(prefix + "enabled");
                enabled && parse_bool(enabled->text).value_or(false))
                report_.skip(files_, enabled->origin,
                             std::string(effect) + " needs a renderer shaoDe lacks so far");
            for (const auto &[name, value] : hypr.options)
                if (name.starts_with(prefix))
                    used_.insert(name);
        }
        used_.insert("decoration:rounding_power");

        text("input:kb_layout", {"keyboard", "layout"});
        text("input:kb_options", {"keyboard", "options"});
        for (auto [name, why] :
             {std::pair{"input:kb_variant", "keyboard variants are not supported yet"},
              {"input:kb_model", "keyboard models are not supported yet"},
              {"input:kb_rules", "XKB rules are not supported"},
              {"input:kb_file", "keymap files are not supported yet"}})
            if (auto *value = option(name); value && !value->text.empty())
                report_.skip(files_, value->origin,
                             std::string(name) + " = " + value->text + ": " + why);
        whole("input:repeat_rate", {"keyboard", "repeat_rate"}, 0, 100);
        whole("input:repeat_delay", {"keyboard", "repeat_delay"}, 0, 5000);
        fraction("input:sensitivity", {"mouse", "speed"}, -1, 1);
        if (auto *profile = option("input:accel_profile"); profile && !profile->text.empty()) {
            if (profile->text == "flat" || profile->text == "adaptive")
                theme_.set({"mouse", "acceleration"}, quote(profile->text), profile->origin);
            else
                report_.skip(files_, profile->origin,
                             "acceleration profile '" + profile->text + "' is not supported");
        }
        if (auto *raw = option("input:force_no_accel");
            raw && parse_bool(raw->text).value_or(false) && !theme_.has({"mouse", "acceleration"}))
            theme_.set({"mouse", "acceleration"}, quote("flat"), raw->origin, "force_no_accel");
        flag("input:natural_scroll", {"mouse", "natural_scroll"});
        flag("input:touchpad:natural_scroll", {"touchpad", "natural_scroll"});
        flag("input:touchpad:tap-to-click", {"touchpad", "tap_to_click"});
        flag("input:touchpad:tap_to_click", {"touchpad", "tap_to_click"});
        flag("input:touchpad:disable_while_typing", {"touchpad", "disable_while_typing"});

        for (const auto &monitor : hypr.monitors)
            this->monitor(monitor);
        order();
        rules(hypr.rules);

        std::vector<std::string> unused;
        for (const auto &[name, value] : hypr.options)
            if (!used_.contains(name))
                unused.push_back(name);
        if (!unused.empty()) {
            std::string list;
            for (const auto &name : unused)
                list += (list.empty() ? "" : ", ") + name;
            report_.skip(files_, {hypr.file, 0}, "no shaoDe equivalent: " + list);
        }
    }

    void waybar(const std::map<std::string, HyprValue> &bar) {
        auto get = [&](const char *key) -> const HyprValue * {
            auto it = bar.find(key);
            return it == bar.end() ? nullptr : &it->second;
        };
        auto integer = [&](const char *key, std::vector<std::string> path, int min, int max) {
            auto *value = get(key);
            if (!value)
                return;
            auto parsed = parse_number(value->text);
            if (!parsed || *parsed < min || *parsed > max)
                report_.skip(files_, value->origin,
                             std::string(key) + " " + value->text + " is outside shaoDe's " +
                                 std::to_string(min) + " to " + std::to_string(max));
            else
                theme_.set(std::move(path), std::to_string(std::lround(*parsed)), value->origin);
        };
        if (auto *position = get("position"))
            theme_.set({"shell", "panel_position"}, quote(position->text), position->origin);
        integer("height", {"shell", "panel_height"}, 24, 100);
        for (auto [key, side] : {std::pair{"margin-top", "top"},
                                 {"margin-right", "right"},
                                 {"margin-bottom", "bottom"},
                                 {"margin-left", "left"}})
            integer(key, {"shell", "panel_margin", side}, 0, 200);
        integer("radius", {"shell", "panel_radius"}, 0, 50);
        if (auto *background = get("background"))
            theme_.set({"shell", "panel_color"}, quote(background->text), background->origin);
        if (auto *color = get("color"))
            theme_.set({"shell", "text_color"}, quote(color->text), color->origin);
        if (auto *font = get("font"))
            theme_.set({"shell", "font"}, quote(font->text), font->origin);
        integer("font_size", {"shell", "font_size"}, 6, 48);
    }

    void wallpaper() {
        auto use = [&](const fs::path &path, const Origin &origin) {
            theme_.set({"shell", "wallpaper"}, quote(path.string()), origin);
        };
        if (auto image = files_.cache("hyde/wall.set")) {
            use(*image, {*files_.cache_path("hyde/wall.set"), 0});
            return;
        }
        if (auto file = files_.cache("wal/wal")) {
            auto text = files_.read(*file);
            auto path = text ? trim(*text) : std::string();
            std::error_code error;
            if (!path.empty() && fs::is_regular_file(path, error)) {
                use(path, {*file, 1});
                return;
            }
        }
        if (auto config = files_.find({"hypr/hyprpaper.conf"})) {
            auto text = files_.read(*config).value_or("");
            int line = 0;
            size_t start = 0;
            while (start < text.size()) {
                auto end = std::min(text.find('\n', start), text.size());
                auto current = trim(std::string_view(text).substr(start, end - start));
                start = end + 1;
                ++line;
                auto equals = current.find('=');
                if (equals == std::string::npos)
                    continue;
                auto key = trim(std::string_view(current).substr(0, equals));
                auto value = trim(std::string_view(current).substr(equals + 1));
                if (key == "wallpaper") {
                    auto parts = split(value, ',');
                    value = parts.empty() ? "" : parts.back();
                } else if (key != "path") {
                    continue;
                }
                if (value.starts_with("~/") && std::getenv("HOME"))
                    value = std::string(std::getenv("HOME")) + value.substr(1);
                std::error_code error;
                if (fs::is_regular_file(value, error)) {
                    use(value, {*config, line});
                    return;
                }
            }
        }
    }

  private:
    const HyprValue *option(const std::string &name) {
        auto it = hypr_->options.find(name);
        if (it == hypr_->options.end())
            return nullptr;
        used_.insert(name);
        return &it->second;
    }
    void whole(const char *name, std::vector<std::string> path, int min, int max, int factor = 1,
               std::string note = {}) {
        auto *value = option(name);
        if (!value)
            return;
        auto parts = split(value->text, ' ');
        auto parsed = parts.empty() ? std::nullopt : parse_number(parts[0]);
        if (!parsed) {
            report_.skip(files_, value->origin,
                         std::string(name) + " = " + value->text + " is not a number");
            return;
        }
        if (parts.size() > 1)
            note = (note.empty() ? "" : note + "; ") + "the first of its per-side values";
        auto result = std::lround(*parsed) * factor;
        if (result < min || result > max) {
            report_.skip(files_, value->origin,
                         std::string(name) + " = " + value->text + " is outside shaoDe's range");
            return;
        }
        theme_.set(std::move(path), std::to_string(result), value->origin, std::move(note));
    }
    void fraction(const char *name, std::vector<std::string> path, double min, double max) {
        auto *value = option(name);
        if (!value)
            return;
        auto parsed = parse_number(value->text);
        if (!parsed || *parsed < min || *parsed > max)
            report_.skip(files_, value->origin,
                         std::string(name) + " = " + value->text + " is not a number from " +
                             number(min) + " to " + number(max));
        else
            theme_.set(std::move(path), number(*parsed), value->origin);
    }
    void text(const char *name, std::vector<std::string> path) {
        if (auto *value = option(name); value && !value->text.empty())
            theme_.set(std::move(path), quote(value->text), value->origin);
    }
    void flag(const char *name, std::vector<std::string> path) {
        auto *value = option(name);
        if (!value)
            return;
        if (auto parsed = parse_bool(value->text))
            theme_.set(std::move(path), *parsed ? "true" : "false", value->origin);
        else
            report_.skip(files_, value->origin,
                         std::string(name) + " = " + value->text + " is not a boolean");
    }

    void monitor(const HyprItem &item) {
        auto field = [&](const char *key) -> std::string {
            auto it = item.fields.find(key);
            return it == item.fields.end() ? std::string() : it->second;
        };
        auto name = field("output");
        if (name.empty()) {
            report_.skip(
                files_, item.origin,
                "the rule for unlisted monitors: shaoDe already gives them their preferred mode");
            return;
        }
        if (name.size() >= 32 && !name.starts_with("desc:")) {
            report_.skip(files_, item.origin, "monitor name " + name + " is too long");
            return;
        }
        // A later rule for the same monitor replaces the earlier one, as in Hyprland.
        std::vector<std::string> base{"outputs", "monitors", name};
        theme_.erase(base);
        positions_.erase(name);
        if (!item.enabled)
            return;
        auto at = [&](const char *key) {
            auto path = base;
            path.emplace_back(key);
            return path;
        };
        if (parse_bool(field("disabled")).value_or(false)) {
            theme_.set(at("enabled"), "false", item.origin);
            return;
        }
        static const std::regex mode_pattern(R"((\d{1,5})x(\d{1,5})(@\d+(\.\d+)?)?)");
        auto mode = field("mode");
        if (auto trimmed = mode.ends_with("Hz") ? mode.substr(0, mode.size() - 2) : mode;
            std::regex_match(trimmed, mode_pattern))
            theme_.set(at("mode"), quote(trimmed), item.origin);
        else if (!mode.empty() && mode != "preferred" && mode != "highres" && mode != "highrr" &&
                 mode != "maxwidth")
            report_.skip(files_, item.origin, name + ": mode " + mode + " is not WIDTHxHEIGHT@HZ");
        static const std::regex position_pattern(R"((-?\d{1,5})x(-?\d{1,5}))");
        std::smatch match;
        auto position = field("position");
        if (std::regex_match(position, match, position_pattern)) {
            int x = std::stoi(match[1]), y = std::stoi(match[2]);
            theme_.set(at("position"),
                       "{ x = " + std::to_string(x) + ", y = " + std::to_string(y) + " }",
                       item.origin);
            if (!name.starts_with("desc:"))
                positions_[name] = {x, item.origin};
        } else if (!position.empty() && !position.starts_with("auto")) {
            report_.skip(files_, item.origin, name + ": position " + position + " is not XxY");
        }
        auto scale = field("scale");
        if (auto parsed = parse_number(scale); parsed && *parsed >= 0.25 && *parsed <= 10)
            theme_.set(at("scale"), number(*parsed), item.origin);
        else if (!scale.empty() && scale != "auto")
            report_.skip(files_, item.origin,
                         name + ": scale " + scale + " is not a number from 0.25 to 10");
        if (auto transform = parse_number(field("transform"));
            transform && *transform >= 0 && *transform <= 7)
            theme_.set(at("transform"), std::to_string(static_cast<int>(*transform)), item.origin);
        if (auto vrr = field("vrr"); !vrr.empty()) {
            if (vrr == "0" || vrr == "1" || vrr == "false" || vrr == "true")
                theme_.set(at("vrr"), vrr == "1" || vrr == "true" ? "true" : "false", item.origin);
            else
                report_.skip(files_, item.origin,
                             name + ": vrr " + vrr + " (fullscreen only) is not supported");
        }
        for (const auto &[key, value] : item.fields)
            if (key == "bitdepth" || key == "mirror" || key == "cm" || key == "sdrbrightness" ||
                key == "sdrsaturation" || key == "reserved")
                report_.skip(files_, item.origin, name + ": " + key + " is not supported yet");
    }
    void order() {
        if (positions_.size() < 2)
            return;
        std::vector<std::pair<std::string, std::pair<int, Origin>>> sorted(positions_.begin(),
                                                                           positions_.end());
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
            return a.second.first < b.second.first;
        });
        if (sorted.size() > 8)
            sorted.resize(8);
        std::string list = "{ ";
        for (const auto &[name, _] : sorted)
            list += quote(name) + ", ";
        list += "}";
        theme_.set({"outputs", "order"}, list, sorted.front().second.second,
                   "left to right by position");
    }
    void rules(const std::vector<HyprItem> &items) {
        std::string list;
        Origin first;
        int without = 0;
        for (const auto &item : items) {
            auto opacity = item.fields.find("opacity");
            if (opacity == item.fields.end()) {
                ++without;
                continue;
            }
            if (!item.enabled) {
                report_.skip(files_, item.origin, "window rule is disabled");
                continue;
            }
            std::string pattern;
            bool other_match = false;
            for (const auto &[key, value] : item.fields)
                if (key == "match:class")
                    pattern = value;
                else if (key.starts_with("match:"))
                    other_match = true;
            if (pattern.empty() || other_match) {
                report_.skip(
                    files_, item.origin,
                    "opacity rule matches more than the app ID; shaoDe's rules match app IDs only");
                continue;
            }
            // Hyprland matches the whole class; shaoDe searches, so anchor it.
            if (!pattern.starts_with("^") || !pattern.ends_with("$"))
                pattern = "^(?:" + pattern + ")$";
            try {
                std::regex check(pattern, std::regex::ECMAScript);
            } catch (const std::regex_error &) {
                report_.skip(files_, item.origin,
                             "class pattern " + pattern + " is not a regex shaoDe understands");
                continue;
            }
            std::vector<double> values;
            for (const auto &word : split(opacity->second, ' '))
                if (auto value = parse_number(word))
                    values.push_back(*value);
            if (values.empty() || values[0] < 0.05 || values[0] > 1 ||
                (values.size() > 1 && (values[1] < 0.05 || values[1] > 1))) {
                report_.skip(files_, item.origin,
                             "opacity " + opacity->second +
                                 ": shaoDe keeps windows from 5% to 100% opaque");
                continue;
            }
            if (list.empty())
                first = item.origin;
            list += "    { app_id = " + quote(pattern) + ", opacity = " + number(values[0]);
            if (values.size() > 1)
                list += ", inactive_opacity = " + number(values[1]);
            list += " }, -- " + files_.show(item.origin) + "\n";
        }
        if (without)
            report_.ignored["window rules without opacity"] += without;
        if (!list.empty())
            theme_.set({"windows", "rules"}, "{\n" + list + "}", first);
    }

    const Files &files_;
    Theme &theme_;
    Report &report_;
    const Hyprland *hypr_ = nullptr;
    std::set<std::string> used_;
    std::map<std::string, std::pair<int, Origin>> positions_;
};

bool identifier(const std::string &key) {
    return !key.empty() && !std::isdigit(static_cast<unsigned char>(key[0])) &&
           std::all_of(key.begin(), key.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
           });
}
struct Node {
    std::string key;
    const Entry *entry = nullptr;
    std::vector<Node> children;
};
void emit(const Node &node, const std::string &indent, const Files &files, std::string &out) {
    for (const auto &child : node.children) {
        auto key = identifier(child.key) ? child.key : "[" + quote(child.key) + "]";
        if (!child.entry) {
            out += indent + key + " = {\n";
            emit(child, indent + "    ", files, out);
            out += indent + "},\n";
            continue;
        }
        std::string value;
        for (char c : child.entry->lua)
            value += c == '\n' ? "\n" + indent : std::string(1, c);
        out += indent + key + " = " + value + ",";
        if (child.entry->lua.find('\n') == std::string::npos) {
            out += " -- " + files.show(child.entry->origin);
            if (!child.entry->note.empty())
                out += " (" + child.entry->note + ")";
        }
        out += "\n";
    }
}
std::string render(const Theme &theme, const Files &files) {
    static const std::vector<std::string> sections = {
        "appearance", "keyboard", "mouse", "touchpad", "layout", "windows", "outputs", "shell"};
    Node root;
    for (const auto &entry : theme.entries()) {
        auto *node = &root;
        for (const auto &key : entry.path) {
            auto it = std::find_if(node->children.begin(), node->children.end(),
                                   [&](const Node &child) { return child.key == key; });
            if (it == node->children.end()) {
                node->children.push_back(Node{key, nullptr, {}});
                it = node->children.end() - 1;
            }
            node = &*it;
        }
        node->entry = &entry;
    }
    std::stable_sort(root.children.begin(), root.children.end(), [&](const Node &a, const Node &b) {
        return std::find(sections.begin(), sections.end(), a.key) <
               std::find(sections.begin(), sections.end(), b.key);
    });
    std::string out = "-- Generated by `shaode import " + files.show({files.source(), 0}) +
                      "`. Reimporting replaces this file;\n"
                      "-- init.lua loads it with theme = \"theme.lua\" and overrides any of it.\n"
                      "return {\n";
    emit(root, "    ", files, out);
    return out + "}\n";
}
} // namespace

ImportResult import_dotfiles(const std::filesystem::path &directory) {
    Files files(directory);
    Theme theme;
    Report report;
    Translator translate(files, theme, report);
    std::vector<std::string> read;
    translate.palette();
    if (auto hypr = read_hyprland(files, report)) {
        read.push_back(files.show({hypr->file, 0}));
        translate.hyprland(*hypr);
    }
    auto bar = read_waybar(files, report);
    if (files.find(
            {"waybar/config.jsonc", "waybar/config.json", "waybar/config", "waybar/style.css"}))
        read.push_back(files.show({files.source() / "waybar", 0}));
    translate.waybar(bar);
    translate.wallpaper();
    if (theme.entries().empty() && read.empty())
        throw std::runtime_error("found no Hyprland, Waybar, wallbash, or pywal settings in " +
                                 directory.string());

    ImportResult result;
    result.theme = render(theme, files);
    try {
        (void)parse_config(result.theme, "theme.lua");
    } catch (const std::exception &error) {
        // Keyboard layouts are the one setting only xkbcommon can check.
        if (!theme.has({"keyboard", "layout"}) && !theme.has({"keyboard", "options"}))
            throw std::runtime_error(std::string("generated an invalid theme: ") + error.what());
        report.skipped.push_back("keyboard layout/options: xkbcommon rejected them; not imported");
        theme.erase({"keyboard", "layout"});
        theme.erase({"keyboard", "options"});
        result.theme = render(theme, files);
        (void)parse_config(result.theme, "theme.lua");
    }
    result.imported = static_cast<int>(theme.entries().size());

    auto &out = result.report;
    for (const auto &entry : theme.entries()) {
        std::string path;
        for (const auto &key : entry.path)
            path += (path.empty() ? "" : ".") + key;
        auto value = entry.lua.find('\n') == std::string::npos ? entry.lua : "{ ... }";
        out += "  " + path + " = " + value + "  <- " + files.show(entry.origin);
        if (!entry.note.empty())
            out += " (" + entry.note + ")";
        out += "\n";
    }
    if (!report.skipped.empty()) {
        out += "Not imported:\n";
        for (const auto &line : report.skipped)
            out += "  " + line + "\n";
    }
    if (!report.ignored.empty()) {
        std::string list;
        for (const auto &[what, count] : report.ignored)
            list += (list.empty() ? "" : ", ") + what + " (" + std::to_string(count) + ")";
        out += "Left to shaoDe's own configuration: " + list + "\n";
    }
    return result;
}
} // namespace shaode
