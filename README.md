# shaoDe

A mouse-first Wayland desktop with Lua configuration, floating windows,
edge snapping, and optional tiling. C++ owns configuration and desktop policy;
a C adapter integrates wlroots. A Qt Quick shell adds a desktop, taskbar, and
searchable application launcher.

This is an early development project, not a replacement desktop session yet.

## Build and run

The first working compositor supports real Wayland applications, click-to-focus,
mouse move/resize, configurable shortcuts, half-screen snapping, maximize/restore,
a one-shot grid arrangement, and Lua reload. It uses a TinyWL-derived C adapter
with C++ configuration and placement policy. The shell is under development:
its UI renders in preview mode; live LayerShellQt integration is awaiting testing.

Requirements: CMake 3.25+, C11 and C++20 compilers, pkg-config, Lua 5.4,
xkbcommon, wlroots **0.20.x**, wayland-server, wayland-protocols, and
wayland-scanner. The shell also needs Qt 6.5+ (Core, Gui, Qml, Quick, Quick Controls
Basic, Quick Layouts, and the Wayland platform plugin), LayerShellQt 6.6+, GLib/GIO,
and wayland-client. Tests use Python 3. Ninja is used below.
Gentoo setup and standalone-session instructions are in [docs/gentoo.md](docs/gentoo.md).
The build supports a normal install prefix and DESTDIR staging.

```sh
cmake -S . -B build -G Ninja -DSHAODE_BUILD_COMPOSITOR=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/shaode --config config/init.lua --check-config
./build/shaode --config config/init.lua --exec kitty
```

The compositor opens a nested window in the current Wayland session.
It selects only the Wayland backend; `--headless` selects the headless backend
for testing. The separate `--session` option selects DRM/libinput from a TTY; that physical
backend is experimental: it has run on one laptop (AMD graphics, single built-in
screen), but not yet on NVIDIA or with multiple monitors. Session-file
installation is opt-in with `SHAODE_INSTALL_SESSION=ON`.
Applications launched through `--exec`, startup entries, or bindings inherit
the nested Wayland socket. Commands after `--exec` consume all remaining
arguments; there is no shell expansion. Full builds start the desktop shell
automatically; `--no-shell` or Lua `shell.enabled = false` disables it. Headless
mode never starts the shell automatically. No other applications start
automatically with the example configuration.

The shell has pinned desktop shortcuts (double-click to launch), a taskbar with
window activation/minimization and a right-click window menu, an application
search menu, a clock, and a show-desktop button. Installed applications are read
from desktop entries through GIO. Lua configures panel height, colors, wallpaper,
and pinned commands. Pinned commands run from your home directory. In a nested
session, applications that reuse an existing process or D-Bus service can open
in the host session instead.

Default bindings (edit [config/init.lua](config/init.lua)):

| Input | Action |
| --- | --- |
| Alt + left/right drag | Move / resize a window |
| Alt + Enter | Launch Kitty |
| Alt + Tab | Cycle windows |
| Alt + F4 | Close focused window |
| Alt + Left/Right | Snap to half the output |
| Alt + Up/Down | Maximize / restore saved floating geometry |
| Alt + T | Arrange the current output's windows in a grid |
| Alt + F11 | Toggle fullscreen |
| Ctrl + Alt + 1–4 | Switch to workspace 1–4 |
| Ctrl + Alt + Shift + 1–4 | Move the focused window to workspace 1–4 |
| Ctrl + Alt + Left/Right | Previous / next workspace |
| Alt + Shift + R | Reload Lua configuration |
| Alt + Shift + Escape | Exit the nested compositor |

The host compositor can consume shortcuts before the nested compositor receives
them; edit the Lua bindings if necessary. SIGHUP also requests a reload, and
SIGINT/SIGTERM requests shutdown. A reload does not rerun startup commands.

Initial limitations: tiling is a one-shot arrangement, not persistent automatic
tiling; snapping is keyboard-driven, without edge-drag previews. Decorations
come from clients. Fullscreen covers the panel while the window is focused;
focusing another window lowers it behind the panel until it is refocused. Window
placement during interactive resize is immediate, without waiting for the
client's next buffer. There is no portal integration yet.

Workspaces are shared across outputs; `layout.workspaces` sets how many (1–10).
The taskbar lists windows from every workspace, and activating one switches to its
workspace. Window shortcuts act only on the current workspace.

A control socket runs any Lua action from scripts or other tools:
`shaode msg workspace 2`, `shaode msg tile`, `shaode msg spawn foot`. The query
`shaode msg get workspace` prints the current workspace, and `shaode msg get windows`
prints one tab-separated line per window: workspace, focused, minimized, app ID, and
title. Children of the session find the socket through `SHAODE_SOCKET`. Actions are
refused while the session is locked.

Screen locking uses the standard `ext-session-lock-v1` protocol, so lockers such
as swaylock or gtklock work; bind one with a `spawn` action. The desktop is covered
before the locker draws, only the locker receives input, and if it crashes the
session stays locked until a new locker takes over. Idle notification and idle
inhibition (`ext-idle-notify-v1`, `idle-inhibit-unstable-v1`) let swayidle lock
or blank after inactivity while video players keep the session awake.

X11 applications run through XWayland when wlroots is built with X support and
`Xwayland` is installed. `DISPLAY` is set from the start, but Xwayland only
starts when the first X11 client connects and exits again once idle (Lua
`xwayland = false` disables it; changing it needs a restart). X11 windows take
part in focus, the taskbar, snapping, maximize, and fullscreen like Wayland windows.

wlroots' X11 window manager can leave events unprocessed, which loses the
first window after Xwayland starts. Until wlroots fixes this, shaoDe nudges the
window manager every 250 ms while Xwayland runs. With wlroots patched by
`packaging/patches/wlroots-xwm-drain.patch`, configure with
`-DSHAODE_XWM_WAKER=OFF` to drop the workaround.
Lua currently configures the exposed settings/actions; custom layout functions
and shell widgets are later work.

## Configuration and testing

Lua configuration returns a versioned table. Settings and shortcuts are validated
before application; an invalid reload retains the active configuration. Lua can
use base functions, tables, strings, math, and UTF-8 to compute settings. Process
and file I/O libraries are not exposed. Configuration is user-controlled code;
the evaluator is not a security boundary for untrusted scripts.

Without `--config`, the executable looks for `$XDG_CONFIG_HOME/shaode/init.lua`,
or `~/.config/shaode/init.lua` when `XDG_CONFIG_HOME` is unset, then falls back
to the installed example under the configured data directory. It never creates
or overwrites a personal configuration automatically.

CTest covers configuration validation, layout bounds/non-overlap, and a headless
compositor with real xdg-shell clients. The integration test verifies mapping,
frame callbacks, maximize/restore, unmapping, accepted/rejected reloads, and clean
shutdown in an isolated temporary runtime directory. Shell builds also render
both QML surfaces using Qt's offscreen software backend. No display session is needed.

To build the compositor without Qt, add `-DSHAODE_BUILD_SHELL=OFF`. To work on
the shell UI without LayerShellQt, use an explicit preview build:

```sh
cmake -S . -B build-preview -G Ninja -DSHAODE_SHELL_PREVIEW_ONLY=ON
cmake --build build-preview
ctest --test-dir build-preview --output-on-failure
./build-preview/shaode-shell --config config/init.lua --preview
./build-preview/shaode-shell --config config/init.lua --preview --preview-desktop
```

Preview builds do not install or automatically launch the shell. Preview windows
show the UI and can launch applications, but do not manage windows or reserve
space on the host desktop.

To build only the configuration and placement tests without wlroots:

```sh
cmake -S . -B build-config -G Ninja -DSHAODE_BUILD_COMPOSITOR=OFF
cmake --build build-config
ctest --test-dir build-config --output-on-failure
```

See [docs/verification.md](docs/verification.md) for the actual test results and
remaining limitations.

## Development sequence

1. **Done:** Lua configuration and native build foundation.
2. **Done:** nested wlroots compositor: real applications, focus, move/resize, shortcuts,
   background, snapping, basic tiling, and reload.
3. **In progress:** Qt Quick shell: taskbar, launcher, desktop context menu, wallpaper
   and icons. Preview rendering is verified; live shell verification is next.
4. Persistent per-workspace tiling, drag-to-edge previews, window rules, and Lua
   extension APIs shared by mouse controls and shortcuts.
5. Session integration: multi-monitor policy, notifications, tray,
   portals/screen sharing, power and audio controls.

The compositor targets wlroots 0.20 specifically, because its API changes
between release series. Develop nested inside the existing Wayland session first.

## Upstream references

- [wlroots API](https://wlroots.pages.freedesktop.org/wlroots/)
- [TinyWL 0.20.2](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/0.20.2/tinywl)
- [Lua 5.4 API](https://www.lua.org/manual/5.4/manual.html)

The compositor adapter derives from TinyWL. Its upstream MIT license is
preserved in [vendor/tinywl/LICENSE](vendor/tinywl/LICENSE). No project-wide
license has been selected yet.
