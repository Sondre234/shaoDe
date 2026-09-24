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
pid_t spawn(const shaode::Command &command) {
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
        return -1;
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
    return error ? -1 : pid;
}
struct Runtime {
    std::filesystem::path path;
    shaode::Config config;
    shaode::Command extra_command;
    bool allow_shell = false;
    pid_t shell_pid = -1;

    void start_shell() {
#if SHAODE_HAS_SHELL
        if (!allow_shell || !config.shell.enabled || shell_pid > 0)
            return;
        try {
            auto binary =
                std::filesystem::canonical("/proc/self/exe").parent_path() / "shaode-shell";
            shell_pid = spawn({binary.string(), "-platform", "wayland", "--config", path.string()});
        } catch (const std::exception &error) {
            std::cerr << "Cannot start desktop shell: " << error.what() << '\n';
        }
#endif
    }
    static void child_exited(void *data, int pid) {
        auto &self = *static_cast<Runtime *>(data);
        if (pid == self.shell_pid) {
            self.shell_pid = -1;
            std::cerr << "Desktop shell exited; reload the configuration to restart it\n";
        }
    }

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
            if (self.shell_pid > 0)
                kill(self.shell_pid, SIGHUP);
            else
                self.start_shell();
            std::cerr << "Configuration reloaded: " << self.path << '\n';
            return true;
        } catch (const std::exception &error) {
            std::cerr << "Reload rejected; keeping active configuration: " << error.what() << '\n';
            return false;
        }
    }
    static void startup(void *data) {
        auto &self = *static_cast<Runtime *>(data);
        self.start_shell();
        for (const auto &command : self.config.startup)
            spawn(command);
        if (!self.extra_command.empty())
            spawn(self.extra_command);
    }
};
std::filesystem::path default_config() {
    std::filesystem::path personal;
    if (const auto *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        personal = std::filesystem::path(xdg) / "shaode/init.lua";
    else if (const auto *home = std::getenv("HOME"); home && *home)
        personal = std::filesystem::path(home) / ".config/shaode/init.lua";
    if (!personal.empty() && std::filesystem::exists(personal))
        return personal;
    if (std::filesystem::exists(SHAODE_DEFAULT_CONFIG))
        return SHAODE_DEFAULT_CONFIG;
    throw std::runtime_error(
        "no configuration found; use --config config/init.lua from the source directory");
}
void usage() {
    std::cout
        << "Usage: shaode [--config PATH] [--check-config] [--headless | --session] [--exec "
           "PROGRAM "
           "[ARGS...]]\n"
           "Default: nested Wayland compositor. --session: standalone DRM/libinput on a TTY.\n"
           "Config: $XDG_CONFIG_HOME/shaode/init.lua or ~/.config/shaode/init.lua\n"
           "Falls back to the installed default; use --config config/init.lua in the source tree.\n"
           "--no-shell disables automatic shell startup. Headless mode never starts it "
           "automatically.\n"
           "SIGHUP reloads configuration; SIGINT/SIGTERM exits.\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::filesystem::path path;
        bool check = false;
        bool no_shell = false;
        sh_backend_mode mode = SH_BACKEND_NESTED;
        shaode::Command command;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                usage();
                return 0;
            }
            if (arg == "--version") {
                std::cout << "shaoDe " << SHAODE_VERSION << '\n';
                return 0;
            }
            if (arg == "--config" && i + 1 < argc)
                path = argv[++i];
            else if (arg == "--check-config")
                check = true;
            else if (arg == "--no-shell")
                no_shell = true;
            else if (arg == "--headless" || arg == "--session") {
                if (mode != SH_BACKEND_NESTED)
                    throw std::runtime_error("choose only one backend mode");
                mode = arg == "--headless" ? SH_BACKEND_HEADLESS : SH_BACKEND_SESSION;
            } else if (arg == "--exec" && i + 1 < argc) {
                while (++i < argc)
                    command.emplace_back(argv[i]);
            } else
                throw std::runtime_error("unknown or incomplete option: " + arg);
        }
        if (path.empty())
            path = default_config();
        Runtime runtime{std::filesystem::absolute(path), shaode::load_config(path),
                        std::move(command)};
        runtime.allow_shell = !no_shell && mode != SH_BACKEND_HEADLESS;
        if (check) {
            std::cout << "Configuration valid: " << path << " (" << runtime.config.bindings.size()
                      << " bindings)\n";
            return 0;
        }
        if (mode == SH_BACKEND_NESTED &&
            (!std::getenv("WAYLAND_DISPLAY") || !*std::getenv("WAYLAND_DISPLAY")))
            throw std::runtime_error("a running Wayland session is required (or use --headless)");
        if (mode == SH_BACKEND_SESSION &&
            ((std::getenv("WAYLAND_DISPLAY") && *std::getenv("WAYLAND_DISPLAY")) ||
             (std::getenv("DISPLAY") && *std::getenv("DISPLAY"))))
            throw std::runtime_error("start --session from a TTY or a display manager, outside an "
                                     "existing graphical session");
        const sh_callbacks callbacks{&runtime,        Runtime::settings, Runtime::key,
                                     Runtime::reload, Runtime::startup,  Runtime::child_exited};
        int result = sh_run(&callbacks, mode);
        if (runtime.shell_pid > 0)
            kill(runtime.shell_pid, SIGTERM);
        return result;
    } catch (const std::exception &error) {
        std::cerr << "shaode: " << error.what() << '\n';
        return 1;
    }
}
