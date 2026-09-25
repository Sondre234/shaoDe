// SPDX-License-Identifier: GPL-3.0-or-later
// Reads the look of a Waybar bar: size and placement from its config, colors, radius, and font
// from its GTK CSS.
#include "common.hpp"

#include <algorithm>
#include <cmath>

namespace shaode::import {
namespace {
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), ::tolower);
    return text;
}
// "window#waybar  >  box" -> "window#waybar>box"
std::string normalize_selector(std::string_view selector) {
    std::string result;
    for (auto c : trim(selector)) {
        bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (space) {
            if (!result.empty() && result.back() != ' ' && result.back() != '>')
                result += ' ';
        } else if (c == '>') {
            if (!result.empty() && result.back() == ' ')
                result.pop_back();
            result += c;
        } else {
            result += c;
        }
    }
    return result;
}
// Arguments of "name(a, b, c)" when `value` is such a call.
std::optional<std::vector<std::string>> call(std::string_view value, std::string_view name) {
    if (!value.starts_with(name) || value.size() < name.size() + 2 || value[name.size()] != '(' ||
        value.back() != ')')
        return std::nullopt;
    return split(value.substr(name.size() + 1, value.size() - name.size() - 2), ',');
}
std::optional<double> channel(std::string_view text, double scale) {
    auto value = trim(text);
    if (value.ends_with('%')) {
        auto percent = parse_number(std::string_view(value).substr(0, value.size() - 1));
        return percent ? std::optional(*percent / 100 * scale) : std::nullopt;
    }
    return parse_number(value);
}
uint8_t byte(double value) {
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 255.0)));
}
Color mix(Color a, Color b, double amount) {
    auto blend = [&](uint8_t x, uint8_t y) { return byte(x + (y - x) * amount); };
    return {blend(a.r, b.r), blend(a.g, b.g), blend(a.b, b.b), blend(a.a, b.a)};
}
// The first number of a length ("16px", "16px 16px 0 0"), in pixels.
std::optional<double> pixels(std::string_view value) {
    auto parts = split(value, ' ');
    if (parts.empty())
        return std::nullopt;
    auto first = parts[0];
    double scale = 1;
    if (first.ends_with("px"))
        first.resize(first.size() - 2);
    else if (first.ends_with("pt")) {
        first.resize(first.size() - 2);
        scale = 4.0 / 3;
    }
    auto number = parse_number(first);
    return number ? std::optional(*number * scale) : std::nullopt;
}
} // namespace

std::optional<Color> css_color(std::string_view text, const Css &css, int depth) {
    auto value = lower(trim(text));
    if (depth > 16 || value.empty())
        return std::nullopt;
    if (value[0] == '@') {
        auto it = css.colors.find(value.substr(1));
        return it == css.colors.end() ? std::nullopt : css_color(it->second.text, css, depth + 1);
    }
    if (value[0] == '#')
        return parse_hex(std::string_view(value).substr(1));
    if (value == "transparent")
        return Color{0, 0, 0, 0};
    if (value == "white")
        return Color{255, 255, 255, 255};
    if (value == "black")
        return Color{0, 0, 0, 255};
    for (const char *name : {"rgba", "rgb"})
        if (auto arguments = call(value, name)) {
            if (arguments->size() == 1) // rgb(r g b / a)
                arguments = split((*arguments)[0], ' ');
            arguments->erase(std::remove(arguments->begin(), arguments->end(), "/"),
                             arguments->end());
            if (arguments->size() < 3 || arguments->size() > 4)
                return std::nullopt;
            double parts[4] = {0, 0, 0, 1};
            for (size_t i = 0; i < arguments->size(); ++i) {
                auto part = channel((*arguments)[i], i < 3 ? 255 : 1);
                if (!part)
                    return std::nullopt;
                parts[i] = *part;
            }
            return Color{byte(parts[0]), byte(parts[1]), byte(parts[2]), byte(parts[3] * 255)};
        }
    if (auto arguments = call(value, "alpha"); arguments && arguments->size() == 2) {
        auto color = css_color((*arguments)[0], css, depth + 1);
        auto factor = parse_number((*arguments)[1]);
        if (!color || !factor)
            return std::nullopt;
        color->a = byte(color->a * *factor);
        return color;
    }
    if (auto arguments = call(value, "mix"); arguments && arguments->size() == 3) {
        auto a = css_color((*arguments)[0], css, depth + 1);
        auto b = css_color((*arguments)[1], css, depth + 1);
        auto amount = parse_number((*arguments)[2]);
        if (!a || !b || !amount)
            return std::nullopt;
        return mix(*a, *b, *amount);
    }
    for (auto [name, fixed] : {std::pair{"shade", 0.0}, {"lighter", 1.3}, {"darker", 0.7}})
        if (auto arguments = call(value, name)) {
            if (arguments->empty() || arguments->size() != (fixed ? 1u : 2u))
                return std::nullopt;
            auto color = css_color((*arguments)[0], css, depth + 1);
            auto factor = fixed ? std::optional(fixed) : parse_number((*arguments)[1]);
            if (!color || !factor)
                return std::nullopt;
            return Color{byte(color->r * *factor), byte(color->g * *factor),
                         byte(color->b * *factor), color->a};
        }
    return std::nullopt;
}

void parse_css(const Files &files, const fs::path &path, Css &css, Report &report, int depth) {
    auto contents = files.read(path);
    if (!contents) {
        report.skip(files, {path, 0}, "cannot read, or outside the imported directory");
        return;
    }
    // Blank out comments but keep their line breaks, so lines stay countable.
    std::string text = *contents;
    for (size_t at = text.find("/*"); at != std::string::npos; at = text.find("/*", at)) {
        auto end = text.find("*/", at + 2);
        end = end == std::string::npos ? text.size() : end + 2;
        for (size_t i = at; i < end; ++i)
            if (text[i] != '\n')
                text[i] = ' ';
        at = end;
    }
    auto line_of = [&](size_t offset) {
        return 1 + static_cast<int>(
                       std::count(text.begin(), text.begin() + static_cast<long>(offset), '\n'));
    };
    size_t at = 0;
    while (at < text.size()) {
        at = text.find_first_not_of(" \t\r\n", at);
        if (at == std::string::npos)
            break;
        if (text[at] == '@') {
            auto semicolon = text.find(';', at);
            auto brace = text.find('{', at);
            if (brace < semicolon) { // @media and friends: skip the block
                int level = 0;
                for (at = brace; at < text.size(); ++at) {
                    level += text[at] == '{' ? 1 : text[at] == '}' ? -1 : 0;
                    if (level == 0)
                        break;
                }
                ++at;
                continue;
            }
            if (semicolon == std::string::npos)
                semicolon = text.size();
            auto statement = trim(std::string_view(text).substr(at, semicolon - at));
            Origin origin{path, line_of(at)};
            at = semicolon + 1;
            if (statement.starts_with("@define-color")) {
                auto rest = trim(std::string_view(statement).substr(13));
                auto space = rest.find_first_of(" \t");
                if (space != std::string::npos)
                    css.colors[rest.substr(0, space)] = {trim(rest.substr(space)), origin};
            } else if (statement.starts_with("@import")) {
                auto target = trim(std::string_view(statement).substr(7));
                if (target.starts_with("url(") && target.ends_with(")"))
                    target = trim(std::string_view(target).substr(4, target.size() - 5));
                if (target.size() >= 2 && (target[0] == '"' || target[0] == '\''))
                    target = target.substr(1, target.size() - 2);
                if (depth >= 16)
                    report.skip(files, origin, "@import nested too deeply");
                else if (auto file = files.resolve(target, path.parent_path()))
                    parse_css(files, *file, css, report, depth + 1);
                else
                    report.skip(files, origin,
                                "@import " + target + ": missing or outside the directory");
            }
            continue;
        }
        auto open = text.find('{', at);
        if (open == std::string::npos)
            break;
        auto close = text.find('}', open);
        if (close == std::string::npos)
            close = text.size();
        auto selectors = split(std::string_view(text).substr(at, open - at), ',');
        size_t start = open + 1;
        while (start < close) {
            auto end = std::min(text.find(';', start), close);
            auto declaration = std::string_view(text).substr(start, end - start);
            if (auto colon = declaration.find(':'); colon != std::string_view::npos) {
                Origin origin{path, line_of(start + declaration.find_first_not_of(" \t\r\n"))};
                auto property = lower(trim(declaration.substr(0, colon)));
                auto value = trim(declaration.substr(colon + 1));
                if (auto important = value.find("!important"); important != std::string::npos)
                    value = trim(std::string_view(value).substr(0, important));
                for (const auto &selector : selectors)
                    css.declarations.push_back(
                        {normalize_selector(selector), property, value, origin});
            }
            start = end + 1;
        }
        at = close + 1;
    }
}

std::map<std::string, HyprValue> read_waybar(const Files &files, Report &report) {
    std::map<std::string, HyprValue> result;
    if (auto config = files.find({"waybar/config.jsonc", "waybar/config.json", "waybar/config"})) {
        try {
            auto text = files.read(*config);
            auto json = parse_json(text ? *text : "");
            if (json.kind == Json::Array) {
                if (json.items.size() > 1)
                    report.skip(files, {*config, 0}, "several bars; only the first is imported");
                json = json.items.empty() ? Json{} : json.items[0];
            }
            auto origin = [&](const Json &value) { return Origin{*config, value.line}; };
            if (auto *position = json.get("position"); position && position->kind == Json::String) {
                if (position->text == "top" || position->text == "bottom")
                    result["position"] = {position->text, origin(*position)};
                else
                    report.skip(files, origin(*position),
                                "a " + position->text + " bar is not supported");
            }
            if (auto *height = json.get("height"); height && height->kind == Json::Number)
                result["height"] = {std::to_string(std::lround(height->number)), origin(*height)};
            if (auto *margin = json.get("margin")) {
                auto values =
                    margin->kind == Json::Number
                        ? std::vector<std::string>{std::to_string(std::lround(margin->number))}
                        : split(margin->text, ' ');
                // CSS shorthand: 1 to 4 values, top right bottom left.
                static const int pick[4][4] = {
                    {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
                if (!values.empty() && values.size() <= 4) {
                    const char *sides[] = {"margin-top", "margin-right", "margin-bottom",
                                           "margin-left"};
                    for (int i = 0; i < 4; ++i)
                        result[sides[i]] = {values[static_cast<size_t>(pick[values.size() - 1][i])],
                                            origin(*margin)};
                }
            }
            for (const char *side : {"margin-top", "margin-right", "margin-bottom", "margin-left"})
                if (auto *margin = json.get(side); margin && margin->kind == Json::Number)
                    result[side] = {std::to_string(std::lround(margin->number)), origin(*margin)};
        } catch (const std::exception &error) {
            report.skip(files, {*config, 0}, std::string("not valid JSON: ") + error.what());
        }
    }

    auto style = files.find({"waybar/style.css"});
    if (!style)
        return result;
    Css css;
    parse_css(files, *style, css, report);
    // HyDE-style bars paint `window#waybar > box`; plain ones paint `window#waybar`. The
    // universal selector only counts for the font.
    struct Found {
        std::optional<HyprValue> value;
        std::optional<Color> color;
    };
    std::map<std::string, Found> window, box, any;
    for (const auto &declaration : css.declarations) {
        auto *target = declaration.selector == "window#waybar"       ? &window
                       : declaration.selector == "window#waybar>box" ? &box
                       : declaration.selector == "*"                 ? &any
                                                                     : nullptr;
        if (!target)
            continue;
        HyprValue value{declaration.value, declaration.origin};
        const auto &property = declaration.property;
        if (property == "background" || property == "background-color") {
            std::optional<Color> color;
            if (lower(declaration.value) == "none")
                color = Color{0, 0, 0, 0};
            for (const auto &part : split(declaration.value, ' '))
                if (!color)
                    color = css_color(part, css);
            if (color)
                (*target)["background"] = {value, color};
            else if (property == "background-color")
                report.skip(files, declaration.origin,
                            "cannot read the color " + declaration.value);
        } else if (property == "color") {
            if (auto color = css_color(declaration.value, css))
                (*target)["color"] = {value, color};
            else
                report.skip(files, declaration.origin,
                            "cannot read the color " + declaration.value);
        } else if (property == "border-radius" || property == "font-family" ||
                   property == "font-size") {
            (*target)[property] = {value, std::nullopt};
        }
    }
    auto pick = [&](const char *property, bool universal) -> const Found * {
        for (auto *source : {&box, &window, universal ? &any : nullptr})
            if (source && source->contains(property))
                return &source->at(property);
        return nullptr;
    };
    // A transparent box over a painted window shows the window.
    const Found *background = nullptr;
    if (box.contains("background") && box["background"].color->a > 0)
        background = &box["background"];
    else if (window.contains("background"))
        background = &window["background"];
    else if (box.contains("background"))
        background = &box["background"];
    if (background)
        result["background"] = {hex(*background->color), background->value->origin};
    if (auto *color = pick("color", true))
        result["color"] = {hex(*color->color), color->value->origin};
    const Found *radius = nullptr;
    if (box.contains("border-radius") && pixels(box["border-radius"].value->text).value_or(0) > 0)
        radius = &box["border-radius"];
    else
        radius = pick("border-radius", false);
    if (radius) {
        if (auto value = pixels(radius->value->text))
            result["radius"] = {std::to_string(std::lround(*value)), radius->value->origin};
        else
            report.skip(files, radius->value->origin,
                        "cannot read the radius " + radius->value->text);
    }
    if (auto *font = pick("font-family", true)) {
        auto families = split(font->value->text, ',');
        auto family = families.empty() ? std::string() : families[0];
        if (family.size() >= 2 && (family[0] == '"' || family[0] == '\''))
            family = family.substr(1, family.size() - 2);
        if (!family.empty())
            result["font"] = {family, font->value->origin};
    }
    if (auto *size = pick("font-size", true)) {
        if (auto value = pixels(size->value->text))
            result["font_size"] = {std::to_string(std::lround(*value)), size->value->origin};
        else
            report.skip(files, size->value->origin,
                        "cannot read the font size " + size->value->text);
    }
    return result;
}
} // namespace shaode::import
