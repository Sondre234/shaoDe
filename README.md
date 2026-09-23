# shaoDe

A mouse-first Wayland desktop with Lua configuration, floating windows,
edge snapping, and optional tiling. C++ owns configuration and desktop policy;
a C adapter integrates wlroots. A Qt Quick desktop shell is planned.

This is an early development project, not a replacement desktop session yet.

## Build and run

The first working compositor supports real Wayland applications, click-to-focus,
mouse move/resize, configurable shortcuts, half-screen snapping, maximize/restore,
a one-shot grid arrangement, and Lua reload. It uses a TinyWL-derived C adapter
with C++ configuration and placement policy. The Qt shell is not implemented yet.

Requirements: CMake 3.25+, C11 and C++20 compilers, pkg-config, Lua 5.4,
xkbcommon, wlroots **0.20.x**, wayland-server, wayland-protocols, and
wayland-scanner. Tests also use Python 3 and wayland-client. Ninja is used below.
Arch provides wlroots as `wlroots0.20`.

```sh
cmake -S . -B build -G Ninja -DSHAODE_BUILD_COMPOSITOR=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/shaode --config config/init.lua --check-config
./build/shaode --config config/init.lua --exec kitty
```

The compositor opens a nested window in the current Wayland session.
It selects only the Wayland backend; `--headless` selects the headless backend
for testing. It does not install a login session or start a DRM backend.
Applications launched through `--exec`, startup entries, or bindings inherit
the nested Wayland socket. Commands after `--exec` consume all remaining
arguments; there is no shell expansion. No applications start automatically
with the example configuration.

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
| Alt + Shift + R | Reload Lua configuration |
| Alt + Shift + Escape | Exit the nested compositor |

The host compositor can consume shortcuts before the nested compositor receives
them; edit the Lua bindings if necessary. SIGHUP also requests a reload, and
SIGINT/SIGTERM requests shutdown. A reload does not rerun startup commands.

Initial limitations: tiling is a one-shot arrangement, not persistent automatic
tiling; snapping is keyboard-driven, without edge-drag previews. Decorations
come from clients, and fullscreen requests are not yet implemented. Window
placement during interactive resize is immediate, without waiting for the
client's next buffer. There is no taskbar, launcher UI, workspaces, XWayland,
lock screen, portal integration, or support for using this as a daily desktop.
Lua currently configures the exposed settings/actions; custom layout functions
and shell widgets are later work.

## Configuration and testing

Lua configuration returns a versioned table. Settings and shortcuts are validated
before application; an invalid reload retains the active configuration. Lua can
use base functions, tables, strings, math, and UTF-8 to compute settings. Process
and file I/O libraries are not exposed. Configuration is user-controlled code;
the evaluator is not a security boundary for untrusted scripts.

Without `--config`, the executable looks for `$XDG_CONFIG_HOME/shaode/init.lua`,
or `~/.config/shaode/init.lua` when `XDG_CONFIG_HOME` is unset. It never creates
or overwrites a personal configuration automatically.

CTest covers configuration validation, layout bounds/non-overlap, and a headless
compositor with real xdg-shell clients. The integration test verifies mapping,
frame callbacks, maximize/restore, unmapping, accepted/rejected reloads, and clean
shutdown in an isolated temporary runtime directory. No display session is needed.

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
3. **Next:** Qt Quick shell: taskbar, launcher, desktop context menu, wallpaper and icons.
4. Persistent per-workspace tiling, drag-to-edge previews, window rules, and Lua
   extension APIs shared by mouse controls and shortcuts.
5. Session integration: multi-monitor policy, XWayland, notifications, tray,
   portals/screen sharing, locking, power and audio controls.

The compositor targets wlroots 0.20 specifically, because its API changes
between release series. Develop nested inside the existing Wayland session first.

## Upstream references

- [wlroots API](https://wlroots.pages.freedesktop.org/wlroots/)
- [TinyWL 0.20.2](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/0.20.2/tinywl)
- [Lua 5.4 API](https://www.lua.org/manual/5.4/manual.html)

The compositor adapter derives from TinyWL. Its upstream MIT license is
preserved in [vendor/tinywl/LICENSE](vendor/tinywl/LICENSE). No project-wide
license has been selected yet.
