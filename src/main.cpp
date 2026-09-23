#include "shaode/config.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <utility>

extern char **environ;
namespace {
void spawn(const shaode::Command &command) {
    std::vector<char *> argv;
    for (const auto &arg : command)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    // Wayland's signal event sources block signals in the compositor. Children
    // need an ordinary signal mask so their own shutdown handling still works.
    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (error) {
        std::cerr << "Cannot prepare child process: " << std::strerror(error) << '\n';
        return;
    }
    sigset_t mask;
    sigemptyset(&mask);
    error = posix_spawnattr_setsigmask(&attributes, &mask);
    if (!error)
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
    if (!error)
        error = posix_spawnp(&pid, argv[0], nullptr, &attributes, argv.data(), environ);
    posix_spawnattr_destroy(&attributes);
    if (error)
        std::cerr << "Cannot launch " << command.front() << ": " << std::strerror(error) << '\n';
}
struct Runtime {
    std::filesystem::path path;
    shaode::Config config;
    shaode::Command extra_command;

    static const sh_settings *settings(void *data) {
        return &static_cast<Runtime *>(data)->config.settings;
    }
    static sh_action key(void *data, uint32_t modifiers, uint32_t keysym) {
        auto &self = *static_cast<Runtime *>(data);
        auto *binding = self.config.binding(modifiers, keysym);
        if (!binding)
            return SH_NONE;
        if (binding->action == SH_HANDLED)
            spawn(binding->command);
        return binding->action;
    }
    static bool reload(void *data) {
        auto &self = *static_cast<Runtime *>(data);
        try {
            auto next = shaode::load_config(self.path);
            self.config = std::move(next);
            std::cerr << "Configuration reloaded: " << self.path << '\n';
            return true;
        } catch (const std::exception &error) {
            std::cerr << "Reload rejected; keeping active configuration: " << error.what() << '\n';
            return false;
        }
    }
    static void startup(void *data) {
        auto &self = *static_cast<Runtime *>(data);
        for (const auto &command : self.config.startup)
            spawn(command);
        if (!self.extra_command.empty())
            spawn(self.extra_command);
    }
};
std::filesystem::path default_config() {
    if (const auto *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "shaode/init.lua";
    if (const auto *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config/shaode/init.lua";
    throw std::runtime_error("pass --config PATH or set XDG_CONFIG_HOME");
}
void usage() {
    std::cout << "Usage: shaode [--config PATH] [--check-config] [--headless] [--exec PROGRAM "
                 "[ARGS...]]\n"
                 "Default: nested Wayland compositor; no DRM/session takeover.\n"
                 "Config: $XDG_CONFIG_HOME/shaode/init.lua or ~/.config/shaode/init.lua\n"
                 "Use --config config/init.lua from the source directory to get started.\n"
                 "SIGHUP reloads configuration; SIGINT/SIGTERM exits.\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::filesystem::path path;
        bool check = false, headless = false;
        shaode::Command command;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                usage();
                return 0;
            }
            if (arg == "--config" && i + 1 < argc)
                path = argv[++i];
            else if (arg == "--check-config")
                check = true;
            else if (arg == "--headless")
                headless = true;
            else if (arg == "--exec" && i + 1 < argc) {
                while (++i < argc)
                    command.emplace_back(argv[i]);
            } else
                throw std::runtime_error("unknown or incomplete option: " + arg);
        }
        if (path.empty())
            path = default_config();
        Runtime runtime{path, shaode::load_config(path), std::move(command)};
        if (check) {
            std::cout << "Configuration valid: " << path << " (" << runtime.config.bindings.size()
                      << " bindings)\n";
            return 0;
        }
        if (!headless && (!std::getenv("WAYLAND_DISPLAY") || !*std::getenv("WAYLAND_DISPLAY")))
            throw std::runtime_error("a running Wayland session is required (or use --headless)");
        const sh_callbacks callbacks{&runtime, Runtime::settings, Runtime::key, Runtime::reload,
                                     Runtime::startup};
        return sh_run(&callbacks, headless);
    } catch (const std::exception &error) {
        std::cerr << "shaode: " << error.what() << '\n';
        return 1;
    }
}
