// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaode/config.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char **environ;
namespace {
bool has_env(const char *name) {
    const char *value = std::getenv(name);
    return value && *value;
}
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
/* D-Bus-activated services such as xdg-desktop-portal start with the bus's environment, not
 * ours, so screen sharing and file choosers need to learn about this session. Only a standalone
 * session may do this: a nested one would point the host's portals at itself. */
void export_activation_environment() {
    shaode::Command command{"dbus-update-activation-environment", "--systemd"};
    for (const char *name :
         {"WAYLAND_DISPLAY", "DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "SHAODE_SOCKET"})
        if (has_env(name))
            command.emplace_back(name);
    pid_t pid = spawn(command);
    // Wait briefly, so a portal started by the first applications already sees this session.
    for (int tries = 0; pid > 0 && tries < 100; ++tries) {
        int status;
        if (waitpid(pid, &status, WNOHANG) != 0)
            return;
        usleep(20000);
    }
    if (pid > 0)
        std::cerr << "dbus-update-activation-environment is still running; not waiting\n";
}
struct Runtime {
    std::filesystem::path path;
    shaode::Config config;
    shaode::Command extra_command;
    bool allow_shell = false;
    bool standalone = false;
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
    static sh_action key(void *data, uint32_t modifiers, uint32_t keysym, int *argument) {
        auto &self = *static_cast<Runtime *>(data);
        auto *binding = self.config.binding(modifiers, keysym);
        if (!binding)
            return SH_NONE;
        if (binding->action == SH_HANDLED)
            spawn(binding->command);
        *argument = binding->workspace;
        return binding->action;
    }
    /* Control requests: "<action> [workspace]" or "spawn PROGRAM [ARGS...]". */
    static sh_action command(void *data, const char *request, int *argument, char *error,
                             size_t error_size) {
        auto &self = *static_cast<Runtime *>(data);
        std::istringstream stream(request);
        std::vector<std::string> words{std::istream_iterator<std::string>(stream), {}};
        try {
            if (words.empty())
                throw std::runtime_error("empty request");
            sh_action action = shaode::parse_action(words[0]);
            if (action == SH_HANDLED) {
                if (words.size() < 2)
                    throw std::runtime_error("spawn needs a program");
                if (spawn({words.begin() + 1, words.end()}) < 0)
                    throw std::runtime_error("cannot launch " + words[1]);
                return action;
            }
            if (shaode::action_takes_workspace(action)) {
                std::size_t used = 0;
                int number = words.size() == 2 ? std::stoi(words[1], &used) : 0;
                if (words.size() != 2 || used != words[1].size() || number < 1 ||
                    number > self.config.settings.workspaces)
                    throw std::runtime_error(words[0] + " needs a workspace from 1 to " +
                                             std::to_string(self.config.settings.workspaces));
                *argument = number;
            } else if (words.size() != 1) {
                throw std::runtime_error(words[0] + " takes no argument");
            }
            return action;
        } catch (const std::exception &failure) {
            std::snprintf(error, error_size, "%s", failure.what());
            return SH_NONE;
        }
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
        if (self.standalone)
            export_activation_environment();
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
/* `shaode msg ...` sends one request to the running compositor's control socket. */
int send_message(int argc, char **argv) {
    std::string request;
    for (int i = 2; i < argc; ++i)
        request += (i > 2 ? " " : "") + std::string(argv[i]);
    if (request.empty() || request.find('\n') != std::string::npos)
        throw std::runtime_error(
            "usage: shaode msg ACTION [ARGUMENT] | get workspace|tiling|windows");
    const char *path = std::getenv("SHAODE_SOCKET");
    if (!path || !*path)
        throw std::runtime_error("SHAODE_SOCKET is not set; run inside a shaoDe session");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (std::strlen(path) >= sizeof(address.sun_path))
        throw std::runtime_error("control socket path is too long");
    std::strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        if (fd >= 0)
            close(fd);
        throw std::runtime_error(std::string("cannot connect to ") + path + ": " +
                                 std::strerror(errno));
    }
    request += '\n';
    for (size_t sent = 0; sent < request.size();) {
        ssize_t written = write(fd, request.data() + sent, request.size() - sent);
        if (written <= 0) {
            close(fd);
            throw std::runtime_error("cannot send request");
        }
        sent += static_cast<size_t>(written);
    }
    std::string reply;
    char buffer[4096];
    for (ssize_t count; (count = read(fd, buffer, sizeof(buffer))) > 0;)
        reply.append(buffer, static_cast<size_t>(count));
    close(fd);
    bool ok = reply.rfind("ok", 0) == 0;
    auto body = reply.substr(std::min(reply.find('\n') + 1, reply.size()));
    if (ok)
        std::cout << body;
    else
        std::cerr << "shaode: " << (reply.empty() ? "no reply\n" : reply);
    return ok ? 0 : 1;
}
void usage() {
    std::cout
        << "Usage: shaode [--config PATH] [--check-config] [--headless | --session] "
           "[--exec PROGRAM [ARGS...]]\n"
           "Default: nested Wayland compositor. --session: standalone DRM/libinput on a TTY.\n"
           "Config: $XDG_CONFIG_HOME/shaode/init.lua or ~/.config/shaode/init.lua\n"
           "Falls back to the installed default; use --config config/init.lua in the source tree.\n"
           "--no-shell disables automatic shell startup; headless mode never starts it.\n"
           "SIGHUP reloads configuration; SIGINT/SIGTERM exits.\n"
           "shaode msg ACTION [ARGUMENT] runs an action in the running session;\n"
           "shaode msg get workspace|tiling|windows prints its state.\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::filesystem::path path;
        bool check = false;
        bool no_shell = false;
        sh_backend_mode mode = SH_BACKEND_NESTED;
        shaode::Command command;
        if (argc >= 2 && std::string(argv[1]) == "msg")
            return send_message(argc, argv);
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
        runtime.standalone = mode == SH_BACKEND_SESSION;
        if (check) {
            std::cout << "Configuration valid: " << path << " (" << runtime.config.bindings.size()
                      << " bindings)\n";
            return 0;
        }
        if (mode == SH_BACKEND_NESTED && !has_env("WAYLAND_DISPLAY"))
            throw std::runtime_error("a running Wayland session is required (or use --headless)");
        if (mode == SH_BACKEND_SESSION && (has_env("WAYLAND_DISPLAY") || has_env("DISPLAY")))
            throw std::runtime_error("start --session from a TTY or a display manager, outside an "
                                     "existing graphical session");
        const sh_callbacks callbacks{
            &runtime,        Runtime::settings, Runtime::key,         Runtime::command,
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
