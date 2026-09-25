# Running shaoDe on Gentoo

shaoDe is being developed for a personal Gentoo desktop. It currently has a
working nested compositor and an experimental standalone backend. The physical
DRM/input session has run on an Arch laptop; the actual Gentoo build has only been
tested in the VM below. There is no dependency on systemd in shaoDe itself.
For a reproducible Gentoo test machine, see the QEMU/KVM VM scripts in
[tools/gentoo-vm](../tools/gentoo-vm/README.md).

## Dependencies

Use the wlroots **0.20** slot, not an arbitrary newer release. Its C API changes
between release series. Relevant Gentoo packages are:

- `gui-libs/wlroots:0.20`
- `dev-lang/lua:5.4`
- `dev-libs/wayland` and `dev-libs/wayland-protocols`
- `dev-util/wayland-scanner` and `x11-libs/libxkbcommon`
- `dev-qt/qtbase:6` (with its default `network` USE flag), `dev-qt/qtdeclarative:6`,
  and `dev-qt/qtwayland:6`
- `kde-plasma/layer-shell-qt:6` (6.6+) and `dev-libs/glib:2` for the desktop shell
- `dev-build/cmake`, `dev-build/ninja`, and `virtual/pkgconfig`
- A compiler supporting C11 and C++20
- Python 3 for the automated tests; a Wayland terminal for interactive testing
- Optional: `gui-apps/swaylock` (or another ext-session-lock locker) and
  `gui-apps/swayidle` for locking and idle timeouts
- For screen sharing (Discord, browsers, OBS) and file choosers:
  `gui-libs/xdg-desktop-portal-wlr`, `sys-apps/xdg-desktop-portal-gtk`,
  `media-video/pipewire` running in the user session, and `gui-apps/slurp` for choosing
  a monitor. Without systemd, start the session under `dbus-run-session` as shown below so
  portals can be activated.

Standalone operation requires wlroots built with `drm`, `libinput`, and `session`
USE flags. X11 applications additionally need wlroots with the `X` USE flag and
`x11-base/xwayland`; shaoDe then links `x11-libs/libxcb` and enables XWayland
automatically. To fix wlroots' lost X11 windows at the source, copy
`packaging/patches/wlroots-xwm-drain.patch` into `/etc/portage/patches/gui-libs/wlroots/`,
re-emerge wlroots, and configure shaoDe with `-DSHAODE_XWM_WAKER=OFF`. A GLES2-capable graphics stack is needed. The upstream Gentoo ebuild
lists the backend dependencies and flags:
[wlroots 0.20.2 ebuild](https://github.com/gentoo/gentoo/blob/master/gui-libs/wlroots/wlroots-0.20.2.ebuild).
Check the package version/keywords available in your own tree before installing.

For example, review these with Portage on the Gentoo machine:

```sh
emerge --ask gui-libs/wlroots:0.20 dev-lang/lua:5.4 \
    dev-libs/wayland dev-libs/wayland-protocols dev-util/wayland-scanner \
    x11-libs/libxkbcommon dev-build/cmake dev-build/ninja virtual/pkgconfig \
    dev-qt/qtbase:6 dev-qt/qtdeclarative:6 dev-qt/qtwayland:6 \
    kde-plasma/layer-shell-qt:6 dev-libs/glib:2
```

The shell uses Qt Quick Controls' Basic style and Quick Layouts from
[qtdeclarative](https://packages.gentoo.org/packages/dev-qt/qtdeclarative), the
[Qt Wayland client platform](https://packages.gentoo.org/packages/dev-qt/qtwayland),
and [LayerShellQt](https://packages.gentoo.org/packages/kde-plasma/layer-shell-qt).
It does not require running Plasma. Use Qt with OpenGL/Wayland support for normal
GPU rendering. An installed icon theme supplies application icons; missing icons
have a built-in fallback. `xdg-open` and a file manager are optional for the Home
shortcut; edit the Lua launcher commands to match your installed applications.

These are instructions for the target machine; shaoDe's build does not run
Portage, modify USE flags, install services, or alter your session configuration.

## Build, verify, and install as your user

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build
ctest --test-dir build --output-on-failure
./build/shaode --config config/init.lua --check-config
./build/shaode --config config/init.lua --exec foot
cmake --install build
```

Replace `foot` with an installed Wayland terminal. Change the terminal command in
`config/init.lua` too: the example uses Kitty. The executable uses the installed
example configuration when no personal config exists. To customize it, create
`~/.config/shaode/init.lua` from the example; installation never overwrites this
personal file. For a custom location use `--config /path/to/init.lua`.

The normal build includes `shaode-shell` alongside `shaode`. For compositor-only
development add `-DSHAODE_BUILD_SHELL=OFF`; Qt, GIO, and LayerShellQt are then
unnecessary. `--no-shell` skips shell startup at runtime. Lua's `shell` table
controls its colors, panel height, wallpaper path, and pinned launchers;
`layout.tiling = true` starts with automatic tiling, which the panel button and
Super+Shift+T toggle at runtime. Reload with Super+Shift+R after editing the file. Invalid configuration retains the last
working settings.

`BUILD_TESTING=OFF` omits the test tools/Python requirement. `DESTDIR` staging and
GNU install directories are supported for packaging. The display-manager session
entry is optional and disabled by default (`SHAODE_INSTALL_SESSION=ON` enables
it at install time). No system service is provided or required by the project.

## Standalone session from a TTY

First arrange device access using the session provider appropriate to your
Gentoo installation: elogind/systemd-logind or seatd via libseat. This is a host
configuration choice; shaoDe neither starts nor enables those services. An active
login session must provide a private, writable `XDG_RUNTIME_DIR`. With seatd,
ensure your user has permission to its socket according to your system setup.

From a text login outside any existing graphical session, run as your normal
user:

```sh
dbus-run-session -- "$HOME/.local/bin/shaode" --session
```

`--session` selects DRM and libinput explicitly. The default stays nested;
`--headless` is for tests. Starting `--session` from an environment with `DISPLAY`
or `WAYLAND_DISPLAY` set is rejected to avoid accidental session takeover.
Ctrl+Alt+F1 through F12 request VT switching through wlroots/libseat (Ctrl+AltGr
works too, for keyboards whose only Alt key is Right Alt); the Lua quit
binding is Super+M. Do not launch the compositor with sudo.

Switching to another VT pauses the session; wlroots removes every output until you
return, and shaoDe, the shell, and open windows carry on. On laptops whose F-keys
default to media functions, hold Fn for the VT keys (`sudo chvt N` also works).

For a first run on new hardware, `tools/tty-session-test.sh` runs the build tree's
`--session` with a log at `~/.local/state/shaode/tty-test-latest.log`. It has no
time limit; set `SHAODE_TEST_LIMIT` to a number of seconds to quit on its own in
case input does not work.
`SHAODE_TEST_TERM` picks the terminal it opens (default foot).

The backend has run on a laptop (amdgpu, single eDP panel, touchpad) and a
desktop (NVIDIA proprietary driver, three monitors). Hybrid-GPU outputs, hotplug,
and VT switching on NVIDIA are untested. Start it from the console, not over SSH:
a process outside the console's logind session cannot switch VTs.
Keep another TTY available while testing. Set monitor order with `outputs.order`
and `outputs.primary`; per-monitor modes and scaling are not configurable yet; this is not ready to
replace a secured daily session.

References:
[Gentoo wlroots package](https://packages.gentoo.org/packages/gui-libs/wlroots),
[libseat/seatd](https://git.sr.ht/~kennylevinsen/seatd).
