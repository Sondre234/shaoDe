# Verification: first nested compositor checkpoint

Verified on 2026-09-23 with wlroots 0.20.2, GCC 16.2.1, Lua 5.4.8, and Wayland
1.26.0 on Arch Linux. Nested graphics ran inside Hyprland on an NVIDIA RTX 4090
using the wlroots GLES2 renderer. Nothing was installed into the login/session
configuration, and the existing desktop remained running.

## Automated checks

- CMake/Ninja build: passed without compiler warnings.
- Configuration tests: valid/computed settings, key/modifier matching, malformed
  values, unknown settings/actions, duplicate bindings, bounded Lua evaluation,
  and preserving configuration after a failed load.
- Placement tests: output bounds, non-overlap, gaps, negative output origins,
  small outputs, and invalid inputs.
- Headless compositor integration: four actual xdg-shell client connections,
  shared-memory rendering with frame callbacks, maximize/restore, unmap/destroy,
  valid SIGHUP reload, invalid reload rejection with continued rendering,
  invalid `--check-config` exit status, and SIGTERM exit status zero with socket
  removal. Uses Pixman and a private temporary `XDG_RUNTIME_DIR`.
- C/C++ formatting and `git diff --check`: passed.

Run all automated checks with `ctest --test-dir build --output-on-failure`.

## Nested runtime checks

Real Kitty clients were opened inside temporary nested compositor instances.
Input was sent to those windows through Hyprland's documented dispatch API;
window-only screenshots were inspected. Verified:

- Background and application rendering, keyboard text input, window cycling.
- Keyboard half-screen snapping, grid arrangement with two applications,
  maximize and restore.
- Alt + left-button movement and Alt + right-button resizing.
- Alt + F4 closes a client (verified by its process exiting with status zero).
- The host window can be resized; the background follows its dimensions.
- Closing the host window shuts down the compositor with status zero.
- Alt + Shift + Escape terminates the nested compositor.

An oversized initial Kitty window exposed a placement issue. Newly mapped
windows now receive a bounded size suggestion and a reachable initial position.
The updated executable was rebuilt and exercised in the nested runtime checks.
Client minimum-size requirements can still exceed very small outputs.

All temporary test compositor instances and terminals were closed after testing.
Screenshots and host-control helpers were kept outside the repository. The
repository's automated tests do not control the user's desktop.

## Remaining coverage and limitations

Physical-device testing, hotplug, multi-monitor behavior, fractional scaling,
clipboard interoperability, client-side title-bar grabs, and extended soak tests
remain. The compositor checkpoint did not include shell UI, workspaces, persistent tiling,
fullscreen handling, XWayland, portals, or session locking (all were added later;
see the checkpoints below and the README). Interactive resize
positions are applied before clients submit their replacement buffers. This is
a development checkpoint for nested use, not a complete desktop session.

Host input testing followed the
[Hyprland dispatcher documentation](https://wiki.hypr.land/configuring/core/dispatchers/).

## Shell protocol checkpoint

The headless protocol probe now maps an actual layer-shell panel with a
48-pixel exclusive zone and verifies that maximization leaves that area free.
It also receives the window title through foreign-toplevel-management and
exercises minimize, restore/activate, close, and handle removal after unmapping.
The integration test repeats those operations across configuration reloads.

## Qt shell preview checkpoint

On 2026-09-24, the preview build compiles with Qt 6.11.2 and GIO 2.88.3. CTest
renders the taskbar/launcher and desktop QML into PNGs with the offscreen software
backend, using an isolated application directory. Both previews were also
visually inspected. Configuration tests cover shell colors, height, wallpaper,
launchers, and malformed values. The compositor-only build remains supported.

The shell's actual Qt task model also connects to the headless compositor and
controls a real xdg-shell client. Its test verifies title/app ID, active state,
click-to-minimize, restore/activate, maximize/restore, show desktop, close, and
row removal. Stale task IDs after closing a window are harmless. All five CTest
checks pass in the preview build.

At this checkpoint LayerShellQt was not yet installed, so the live shell had not
run. It has since run on physical hardware; see the next section.
The lower-level compositor protocols are tested separately as described above.

## First physical session

On 2026-09-24 `--session` ran from tty3 on an Arch laptop (AMD Vega 8 through
amdgpu, 1920×1080 eDP panel, touchpad; an NVIDIA GTX 1650 was present but
unused). libseat opened the seat through logind. The user checked rendering,
touchpad and keyboard input, launching and moving/resizing windows, the shell's
panel and launcher, and an XWayland application (Discord).

Switching to another VT made shaoDe exit. wlroots 0.20 destroys every DRM output
when the session pauses and recreates them on resume; the compositor took the last
output's removal to mean its nested window had closed, and the shell quit when its
last view closed. After the fix, repeated `chvt` round trips kept the compositor,
shell, and clients alive, and the panel returned on the recreated output. Output
removal logs `Failed to disable CRTC` while the DRM FD is paused; it is harmless.

The same day it ran on an Arch desktop: NVIDIA RTX 4090 with the proprietary
610.57 driver (`nvidia_drm.modeset=1`), wlroots 0.20.2 on the GLES2 renderer,
and three monitors (HDMI-A-1 and DP-3 at 2560×1440, DP-1 at 1920×1080). No
NVIDIA workaround variables were needed. All three screens lit at native
resolution; the user checked the cursor crossing between them, keyboard input,
launching and moving windows, and the panel and taskbar on each screen. The
session was started from tty3, since SDDM's Xorg holds tty2 there, and Xwayland
fell back from the `:0` socket SDDM owns.

The test found three problems, now fixed:

- Every monitor marked a 60 Hz mode as preferred, so 144 and 200 Hz panels ran
  at 60 Hz. Outputs now use the fastest refresh at the preferred resolution
  (200/144/144 Hz on this machine), falling back if the driver rejects it.
- `wlr_output_layout_add_auto` placed monitors in connector order, not their
  physical order. The Lua `outputs.order` and `outputs.primary` settings now set
  it; the user confirmed the layout.
- The taskbar's window menu always opened over the first task button.

These runs were launched over SSH into the tty3 login (`XDG_SESSION_ID`), which
left the process outside that logind session: Ctrl+Alt+F1 was refused
(`Could not switch session: Access denied`), and after the compositor stopped,
the console's keyboard stayed unusable until a reboot. Start `--session` from the
console itself. VT switching on NVIDIA therefore remains unverified.

Clients noted that shaoDe lacks xdg-activation, primary selection, fractional
scaling, and server-side decorations. The first three were added later (see the browser
checkpoint below); X11 windows that ask for decorations now get window controls.

Still untested on hardware: VT switching on NVIDIA, hotplug, suspend/resume, lid
close, and brightness/volume keys.

## Tiling checkpoint

Added 2026-09-24. `ctest` covers the dwindle layout on its own (splits, gaps,
bounds, non-overlap, removal, pointer-side placement, split resizing, separate
trees per output and workspace) and in a headless compositor with real clients:
the toggle, splitting on map, refill after moving a tile to another workspace,
`toggle_floating`, restoring floating sizes, and `subscribe` events. The shell UI
test clicks the panel button against a stand-in control socket. Mouse resizing
and drag-to-retile of tiles have not yet been exercised in a nested or physical
session.

## Browser, Electron, and screen capture checkpoint

Added 2026-09-24. In a headless session on the NVIDIA desktop's GLES2 renderer,
Firefox 154 and Discord 1.0.158 (Electron, both `--ozone-platform=wayland` and via
XWayland) mapped, rendered, and appeared in `shaode msg get windows`, using
linux-dmabuf and explicit sync. `wl-copy --primary` / `wl-paste --primary` round-tripped
the primary selection. `grim` captured the output (wlr-screencopy), and `grim -T`
captured Firefox's window alone through ext-foreign-toplevel-list and the per-window
capture source. `compositor_smoke` checks that every one of these globals is advertised.

Not yet exercised: screen sharing end to end through xdg-desktop-portal-wlr and
PipeWire, the D-Bus environment export in a physical `--session`, drag-and-drop, pointer
lock, and popup placement at output edges.
