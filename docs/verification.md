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
remain. The initial desktop has no shell UI, workspaces, persistent tiling,
fullscreen handling, XWayland, portals, or session locking. Interactive resize
positions are applied before clients submit their replacement buffers. This is
a development checkpoint for nested use, not a complete desktop session.

Host input testing followed the
[Hyprland dispatcher documentation](https://wiki.hypr.land/configuring/core/dispatchers/).
