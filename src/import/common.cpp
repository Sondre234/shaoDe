// SPDX-License-Identifier: GPL-3.0-or-later
#include "common.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <tuple>

namespace shaode::import {
namespace {
bool inside(const fs::path &path, const fs::path &root) {
    if (root.empty())
        return false;
    auto [end, _] = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
    return end == root.end();
}
fs::path canonical_or_empty(const fs::path &path) {
    std::error_code error;
    auto result = fs::canonical(path, error);
    return error ? fs::path{} : result;
}
} // namespace

Files::Files(const fs::path &source) : source_(canonical_or_empty(source)) {
    if (source_.empty() || !fs::is_directory(source_))
        throw std::runtime_error("cannot read directory " + source.string());
    if (const char *home = std::getenv("HOME"); home && *home)
        home_ = fs::path(home).lexically_normal();
    if (source_.filename() == ".config")
        cache_ = canonical_or_empty(source_.parent_path() / ".cache");
}

bool Files::allowed(const fs::path &path) const {
    return inside(path, source_) || inside(path, cache_);
}

std::optional<fs::path> Files::resolve(std::string_view text, const fs::path &base) const {
    if (text.empty())
        return std::nullopt;
    fs::path path;
    if (text.starts_with("~/") && !home_.empty())
        path = home_ / text.substr(2);
    else
        path = base / text;
    path = path.lexically_normal();
    // A dotfiles checkout elsewhere still names its files by their installed location.
    if (!home_.empty()) {
        auto config = home_ / ".config";
        if (inside(path, config) && !inside(source_, canonical_or_empty(config)))
            path = source_ / path.lexically_relative(config);
    }
    auto real = canonical_or_empty(path);
    if (real.empty() || !allowed(real))
        return std::nullopt;
    return real;
}

std::optional<std::string> Files::read(const fs::path &path) const {
    auto real = canonical_or_empty(path);
    if (real.empty() || !allowed(real) || !fs::is_regular_file(real))
        return std::nullopt;
    std::ifstream file(real, std::ios::binary);
    std::string text;
    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
        text.append(buffer, static_cast<size_t>(file.gcount()));
        if (text.size() > 1024 * 1024)
            return std::nullopt;
    }
    if (!file.eof())
        return std::nullopt;
    return text;
}

std::optional<fs::path> Files::find(std::initializer_list<const char *> candidates) const {
    for (const char *candidate : candidates)
        if (auto path = resolve(candidate, source_); path && fs::is_regular_file(*path))
            return path;
    return std::nullopt;
}

std::optional<fs::path> Files::cache(const char *relative) const {
    if (cache_.empty())
        return std::nullopt;
    auto path = canonical_or_empty(cache_ / relative);
    if (path.empty() || !fs::is_regular_file(path))
        return std::nullopt;
    return path;
}

std::optional<fs::path> Files::cache_path(const char *relative) const {
    if (cache_.empty())
        return std::nullopt;
    return cache_ / relative;
}

std::string Files::show(const Origin &origin) const {
    auto path = origin.file.string();
    if (!home_.empty() && inside(origin.file, home_))
        path = "~/" + origin.file.lexically_relative(home_).string();
    return origin.line > 0 ? path + ":" + std::to_string(origin.line) : path;
}

void Report::skip(const Files &files, const Origin &origin, const std::string &message) {
    skipped.push_back(files.show(origin) + ": " + message);
}

std::string hex(Color color, bool alpha) {
    char text[10];
    if (alpha && color.a != 255)
        std::snprintf(text, sizeof(text), "#%02x%02x%02x%02x", color.r, color.g, color.b, color.a);
    else
        std::snprintf(text, sizeof(text), "#%02x%02x%02x", color.r, color.g, color.b);
    return text;
}

std::optional<Color> parse_hex(std::string_view digits) {
    if (digits.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos)
        return std::nullopt;
    auto value = [&](size_t at, size_t width) {
        unsigned result = 0;
        std::from_chars(digits.data() + at, digits.data() + at + width, result, 16);
        return static_cast<uint8_t>(width == 1 ? result * 17 : result);
    };
    switch (digits.size()) {
    case 3:
    case 4:
        return Color{value(0, 1), value(1, 1), value(2, 1),
                     digits.size() == 4 ? value(3, 1) : uint8_t{255}};
    case 6:
    case 8:
        return Color{value(0, 2), value(2, 2), value(4, 2),
                     digits.size() == 8 ? value(6, 2) : uint8_t{255}};
    default:
        return std::nullopt;
    }
}

std::string trim(std::string_view text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    int depth = 0;
    char quote = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote) {
            quote = c == quote ? 0 : quote;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            depth = std::max(0, depth - 1);
        } else if (depth == 0 && (separator == ' ' ? c == ' ' || c == '\t' : c == separator)) {
            if (auto part = trim(text.substr(start, i - start)); !part.empty() || separator != ' ')
                parts.push_back(part);
            start = i + 1;
        }
    }
    if (auto part = trim(text.substr(start)); !part.empty() || (separator != ' ' && !parts.empty()))
        parts.push_back(part);
    return parts;
}

std::optional<double> parse_number(std::string_view text) {
    auto trimmed = trim(text);
    if (trimmed.empty())
        return std::nullopt;
    double value = 0;
    auto *begin = trimmed.data() + (trimmed[0] == '+');
    auto [end, error] = std::from_chars(begin, trimmed.data() + trimmed.size(), value);
    if (error != std::errc{} || end != trimmed.data() + trimmed.size())
        return std::nullopt;
    return value;
}

std::optional<bool> parse_bool(std::string_view text) {
    auto value = trim(text);
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    if (value == "true" || value == "yes" || value == "on" || value == "1")
        return true;
    if (value == "false" || value == "no" || value == "off" || value == "0")
        return false;
    return std::nullopt;
}

const Json *Json::get(std::string_view key) const {
    for (const auto &field : fields)
        if (field.key == key)
            return &field.value;
    return nullptr;
}

namespace {
class JsonParser {
  public:
    explicit JsonParser(std::string_view text) : text_(text) {}
    Json document() {
        auto value = parse(0);
        skip();
        if (at_ < text_.size())
            error("unexpected text after the value");
        return value;
    }

  private:
    [[noreturn]] void error(const std::string &message) {
        throw std::runtime_error("line " + std::to_string(line_) + ": " + message);
    }
    void skip() {
        while (at_ < text_.size()) {
            char c = text_[at_];
            if (c == '\n') {
                ++line_;
                ++at_;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++at_;
            } else if (text_.substr(at_, 2) == "//") {
                while (at_ < text_.size() && text_[at_] != '\n')
                    ++at_;
            } else if (text_.substr(at_, 2) == "/*") {
                auto end = text_.find("*/", at_ + 2);
                if (end == std::string_view::npos)
                    error("unterminated comment");
                line_ +=
                    static_cast<int>(std::count(text_.begin() + at_, text_.begin() + end, '\n'));
                at_ = end + 2;
            } else {
                break;
            }
        }
    }
    std::string string() {
        std::string result;
        for (++at_; at_ < text_.size() && text_[at_] != '"'; ++at_) {
            char c = text_[at_];
            if (c == '\n')
                error("line break inside a string");
            if (c != '\\') {
                result += c;
                continue;
            }
            if (++at_ >= text_.size())
                break;
            switch (char escaped = text_[at_]) {
            case 'n':
                result += '\n';
                break;
            case 't':
                result += '\t';
                break;
            case 'r':
                result += '\r';
                break;
            case 'b':
                result += '\b';
                break;
            case 'f':
                result += '\f';
                break;
            case 'u': {
                unsigned code = 0;
                if (at_ + 4 >= text_.size() ||
                    std::from_chars(text_.data() + at_ + 1, text_.data() + at_ + 5, code, 16).ptr !=
                        text_.data() + at_ + 5)
                    error("bad \\u escape");
                at_ += 4;
                if (code < 0x80) {
                    result += static_cast<char>(code);
                } else if (code < 0x800) {
                    result += static_cast<char>(0xc0 | code >> 6);
                    result += static_cast<char>(0x80 | (code & 0x3f));
                } else {
                    result += static_cast<char>(0xe0 | code >> 12);
                    result += static_cast<char>(0x80 | (code >> 6 & 0x3f));
                    result += static_cast<char>(0x80 | (code & 0x3f));
                }
                break;
            }
            default:
                result += escaped;
            }
        }
        if (at_ >= text_.size())
            error("unterminated string");
        ++at_;
        return result;
    }
    Json parse(int depth) {
        if (depth > 64)
            error("nested too deeply");
        skip();
        if (at_ >= text_.size())
            error("unexpected end of input");
        Json value;
        value.line = line_;
        char c = text_[at_];
        if (c == '{' || c == '[') {
            char close = c == '{' ? '}' : ']';
            value.kind = c == '{' ? Json::Object : Json::Array;
            ++at_;
            for (;;) {
                skip();
                if (at_ < text_.size() && text_[at_] == close) {
                    ++at_;
                    return value;
                }
                if (value.kind == Json::Object) {
                    if (at_ >= text_.size() || text_[at_] != '"')
                        error("expected a quoted key");
                    auto key = string();
                    skip();
                    if (at_ >= text_.size() || text_[at_] != ':')
                        error("expected ':'");
                    ++at_;
                    value.fields.push_back({std::move(key), parse(depth + 1)});
                } else {
                    value.items.push_back(parse(depth + 1));
                }
                skip();
                if (at_ < text_.size() && text_[at_] == ',')
                    ++at_;
                else if (at_ >= text_.size() || text_[at_] != close)
                    error(std::string("expected ',' or '") + close + "'");
            }
        }
        if (c == '"') {
            value.kind = Json::String;
            value.text = string();
            return value;
        }
        for (auto [word, kind, truth] : {std::tuple{"true", Json::Bool, true},
                                         {"false", Json::Bool, false},
                                         {"null", Json::Null, false}})
            if (text_.substr(at_, std::strlen(word)) == word) {
                at_ += std::strlen(word);
                value.kind = kind;
                value.boolean = truth;
                return value;
            }
        auto [end, failure] =
            std::from_chars(text_.data() + at_, text_.data() + text_.size(), value.number);
        if (failure != std::errc{})
            error("unexpected character");
        at_ = static_cast<size_t>(end - text_.data());
        value.kind = Json::Number;
        return value;
    }

    std::string_view text_;
    size_t at_ = 0;
    int line_ = 1;
};
} // namespace

Json parse_json(std::string_view text) { return JsonParser(text).document(); }
} // namespace shaode::import
