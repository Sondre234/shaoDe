# Dotfile import

Goal: point shaoDe at a directory of existing dotfiles (Hyprland, Waybar, pywal,
HyDE/wallbash, …) and carry over their *look and hardware settings*: monitors, color
scheme, bar appearance, gaps, borders, rounding, and transparency. Behaviour (key
bindings, dispatchers, scripts that call `hyprctl`) is out of scope; shaoDe keeps its own.

## Design

- `shaode import DIR` finds the files it recognizes, writes a generated `theme.lua`
  next to the configuration, and prints a report: each imported value with the file
  and line it came from, and everything it skipped.
- `init.lua` loads the generated file and can override any of it. Reimporting
  replaces `theme.lua` only; `init.lua` is never touched.
- Hyprland's Lua config (`hyprland.lua`) is code, not data. It runs in a sandbox where
  `hl.*` functions only record their arguments (`hl.bind`, `hl.on`, timers are no-ops;
  no `os.execute`, no writes). Its `io.open` reads stay allowed inside the source
  directory so branches such as `monitor-mode` files still resolve.
- `hyprland.conf` (hyprlang) gets a small parser: `source =`, `$variables`, nested
  `section { }` blocks, `rgba()`/`rgb()`/`0xAARRGGBB` colors.
- Waybar: only the bar's look transfers. Position, height, and margins come from
  `config.jsonc`; background, text color, border radius, and font come from `style.css`
  (`@define-color` and `window#waybar` only). Module layout does not transfer.
- Later: `theme.follow = "DIR"` reimports on reload or when the source files change, so
  wallbash/pywal colors that change with the wallpaper stay in sync.

## Order

1. Monitor settings in shaoDe (mode, refresh, scale, position, disable).
2. Color scheme plus bar look (position, height, margins, radius, font).
3. Window gaps (inner/outer), borders, rounding, and opacity, including per-app opacity.
4. The importer itself: Hyprland Lua sandbox, hyprlang parser, Waybar, wallbash/pywal.
5. Following changing colors (`theme.follow`).

## Settings tracker

Status: **done** = in shaoDe's Lua config, **missing** = needs implementing in
shaoDe before it can be imported, **won't** = deliberately not carried over.
"Importer" says whether `shaode import` reads the setting yet.

### Monitors

| Setting | Source | shaoDe setting | Status | Importer |
| --- | --- | --- | --- | --- |
| Left-to-right order | `monitor =` positions | `outputs.order` | done | no |
| Primary output | — | `outputs.primary` | done | no |
| Resolution and refresh | `monitor = NAME, 2560x1440@144, …` | `outputs.monitors[NAME].mode` | done | no |
| Position | `monitor = …, 0x0, …` | `outputs.monitors[NAME].position` | done | no |
| Scale | `monitor = …, 1.25` | `outputs.monitors[NAME].scale` | done | no |
| Disable an output | `monitor = NAME, disable` | `outputs.monitors[NAME].enabled` | done | no |
| Transform / rotation | `transform, N` | `outputs.monitors[NAME].transform` | done | no |
| Match by description | `desc:…` | — | missing | no |
| VRR, 10-bit, mirroring | `vrr`, `bitdepth`, `mirror` | — | missing | no |

### Colors and wallpaper

| Setting | Source | shaoDe setting | Status | Importer |
| --- | --- | --- | --- | --- |
| Background color | — | `appearance.background` | done | no |
| Accent / panel / text colors | wallbash, pywal, waybar `@define-color` | `shell.accent`, `panel_color`, `text_color` | done | no |
| Panel transparency | waybar `background: rgba(…)` | `shell.panel_color` as `#RRGGBBAA` | missing | no |
| Wallpaper | hyprpaper, swww, HyDE current wallpaper | `shell.wallpaper` | done | no |
| Font family and size | waybar `font-family`, `font-size` | — | missing | no |

### Bar

| Setting | Source | shaoDe setting | Status | Importer |
| --- | --- | --- | --- | --- |
| Height | waybar `height` | `shell.panel_height` | done | no |
| Top or bottom | waybar `position` | — | missing | no |
| Margins (floating bar) | waybar `margin-*` | — | missing | no |
| Corner radius | `window#waybar { border-radius }` | — | missing | no |
| Module layout | waybar `modules-*` | — | won't | — |

### Windows

| Setting | Source | shaoDe setting | Status | Importer |
| --- | --- | --- | --- | --- |
| Outer gap | `general:gaps_out` | `layout.gap` (one value today) | missing (split) | no |
| Inner gap | `general:gaps_in` | — | missing | no |
| Border width | `general:border_size` | — | missing | no |
| Border colors (active/inactive) | `col.active_border`, `col.inactive_border` | — | missing | no |
| Gradient borders | `rgba(…) rgba(…) 45deg` | — | missing | no |
| Corner rounding | `decoration:rounding` | — | missing | no |
| Active / inactive opacity | `decoration:active_opacity`, `inactive_opacity` | — | missing | no |
| Per-app opacity | `windowrule = opacity A B, class:…` | — | missing | no |
| Blur | `decoration:blur` | — | missing | no |
| Shadows | `decoration:shadow` | — | missing | no |
| Animations | `animation`, `bezier` | — | won't (for now) | — |

### Input

| Setting | Source | shaoDe setting | Status | Importer |
| --- | --- | --- | --- | --- |
| Keyboard layout, options | `input:kb_layout`, `kb_options` | `keyboard.layout`, `options` | done | no |
| Repeat rate / delay | `input:repeat_rate`, `repeat_delay` | `keyboard.repeat_rate`, `repeat_delay` | done | no |
| Pointer speed, acceleration | `input:sensitivity`, `accel_profile` | — | missing | no |
| Natural scroll, tap-to-click | `input:natural_scroll`, `touchpad:tap-to-click` | — | missing | no |

### Out of scope

Key bindings, dispatchers, `exec`/`exec-once`, window rules other than opacity, layer
rules, gestures, permissions, and anything that talks to `hyprctl`.
