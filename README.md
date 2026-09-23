# shaoDe

A mouse-first Wayland desktop with Lua configuration, floating windows,
edge snapping, and optional tiling. C++ owns configuration and desktop policy;
a C adapter integrates wlroots. A Qt Quick desktop shell is planned.

This is an early development project, not a replacement desktop session yet.

## First checkpoint: configuration

Implemented: a versioned Lua configuration loader, strict setting validation,
keyboard binding matching, and transactional configuration loading. Lua can use
base functions, tables, strings, math, and UTF-8 to compute settings. Process and
file I/O libraries are not exposed. Commands use argument arrays, not shell
strings. Configuration is user-controlled code; the evaluator is not a security
boundary for untrusted scripts.

Requirements: CMake 3.25+, a C++20 compiler, pkg-config, Lua 5.4, and xkbcommon.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

See [config/init.lua](config/init.lua) for the planned initial compositor
settings and bindings. At this checkpoint they can be parsed and validated;
the compositor executable is the next checkpoint.

## Development sequence

1. Lua configuration and native build foundation.
2. Nested wlroots compositor: real applications, focus, move/resize, shortcuts,
   background, snapping, basic tiling, and reload.
3. Qt Quick shell: taskbar, launcher, desktop context menu, wallpaper and icons.
4. Persistent per-workspace tiling, drag-to-edge previews, window rules, and Lua
   extension APIs shared by mouse controls and shortcuts.
5. Session integration: multi-monitor policy, XWayland, notifications, tray,
   portals/screen sharing, locking, power and audio controls.

The compositor will target wlroots 0.20 specifically, because its API changes
between release series. Develop nested inside the existing Wayland session first.

## Upstream references

- [wlroots API](https://wlroots.pages.freedesktop.org/wlroots/)
- [TinyWL 0.20.2](https://gitlab.freedesktop.org/wlroots/wlroots/-/tree/0.20.2/tinywl)
- [Lua 5.4 API](https://www.lua.org/manual/5.4/manual.html)

The compositor adapter will derive from TinyWL. Its upstream MIT license is
preserved in [vendor/tinywl/LICENSE](vendor/tinywl/LICENSE). No project-wide
license has been selected yet.
