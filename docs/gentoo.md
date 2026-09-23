# Running shaoDe on Gentoo

shaoDe is being developed for a personal Gentoo desktop. It currently has a
working nested compositor and an experimental standalone backend. The actual
Gentoo build and physical DRM/input session have not yet been tested; development
verification is on Arch. There is no dependency on systemd in shaoDe itself.

## Dependencies

Use the wlroots **0.20** slot, not an arbitrary newer release. Its C API changes
between release series. Relevant Gentoo packages are:

- `gui-libs/wlroots:0.20`
- `dev-lang/lua:5.4`
- `dev-libs/wayland` and `dev-libs/wayland-protocols`
- `dev-util/wayland-scanner` and `x11-libs/libxkbcommon`
- `dev-build/cmake`, `dev-build/ninja`, and `virtual/pkgconfig`
- A compiler supporting C11 and C++20
- Python 3 for the automated tests; a Wayland terminal for interactive testing

Standalone operation requires wlroots built with `drm`, `libinput`, and `session`
USE flags. A GLES2-capable graphics stack is needed. The upstream Gentoo ebuild
lists the backend dependencies and flags:
[wlroots 0.20.2 ebuild](https://github.com/gentoo/gentoo/blob/master/gui-libs/wlroots/wlroots-0.20.2.ebuild).
Check the package version/keywords available in your own tree before installing.

For example, review these with Portage on the Gentoo machine:

```sh
emerge --ask gui-libs/wlroots:0.20 dev-lang/lua:5.4 \
    dev-libs/wayland dev-libs/wayland-protocols dev-util/wayland-scanner \
    x11-libs/libxkbcommon dev-build/cmake dev-build/ninja virtual/pkgconfig
```

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
Ctrl+Alt+F1 through F12 request VT switching through wlroots/libseat; the Lua quit
binding remains Alt+Shift+Escape. Do not launch the compositor with sudo.

This backend path is compiled but has not been exercised on physical hardware.
Keep another TTY available while testing. Session locking, portals, XWayland,
and full multi-monitor management are not implemented yet; this is not ready to
replace a secured daily session.

References:
[Gentoo wlroots package](https://packages.gentoo.org/packages/gui-libs/wlroots),
[libseat/seatd](https://git.sr.ht/~kennylevinsen/seatd).
