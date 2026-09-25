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

## Using it

```sh
shaode import ~/.config              # writes theme.lua beside ~/.config/shaode/init.lua
shaode import --dry-run ~/.config    # prints theme.lua and the report instead
shaode import --config PATH DIR      # writes theme.lua beside PATH
```

`init.lua` names the theme with `theme = "theme.lua"` (the shipped default does). The theme
fills in every setting `init.lua` leaves out: records merge key by key, while lists such as
`windows.rules` and values `init.lua` sets itself stay as `init.lua` has them. A missing
theme file is not an error. After writing, `shaode import` lists the imported settings that
`init.lua` overrides.

Where settings come from, lowest precedence first:

1. Palette: HyDE's wallbash (`~/.cache/hyde/wallbash/gtk.css`: `pry1` background and panel,
   `txt1` text, `1xa6` accent), else pywal (`wal/colors.json`: background, foreground,
   `color4` accent). `~/.cache` is read only when DIR is a `.config` directory.
2. Hyprland: `hypr/hyprland.lua` when it exists, else `hypr/hyprland.conf`. The active
   border's first color also becomes the accent.
3. Waybar: `waybar/config.jsonc` (first bar only) and `waybar/style.css` with its imports.
   HyDE-style bars that paint `window#waybar > box` over a transparent window are handled.
4. Wallpaper: HyDE's `~/.cache/hyde/wall.set`, else pywal's `~/.cache/wal/wal`, else
   hyprpaper.

Every read stays inside DIR (and the `~/.cache` beside it); absolute `~/.config/…` paths in a
copied dotfiles directory are redirected into the copy. In the Lua sandbox, `hl.config`,
`hl.monitor`, `hl.window_rule`, and `hl.device` are recorded; every other `hl` function and any
undefined global (HyDE's `hyde`, for one) is a stub; `io.open`/`io.lines`/`loadfile`/`dofile`/
`require` read inside DIR only; there is no `os.execute`, `io.popen`, or writing; and memory and
instructions are capped. If the script raises an error, the settings recorded before it still
count. Hyprland window rules match the whole class, so imported patterns are anchored.

## Order

1. Monitor settings in shaoDe (mode, refresh, scale, position, disable).
2. Color scheme plus bar look (position, height, margins, radius, font).
3. Window gaps (inner/outer), borders, rounding, and opacity, including per-app opacity.
4. The importer itself: Hyprland Lua sandbox, hyprlang parser, Waybar, wallbash/pywal. Done.
5. Following changing colors (`theme.follow`).

## Settings tracker

Status: **done** = in shaoDe's Lua config, **missing** = needs implementing in
shaoDe before it can be imported, **won't** = deliberately not carried over.
"Importer" says whether `shaode import` reads the setting yet.

### Monitors

| Setting | Source | shaoDe setting | Status | Importer |
| --- | --- | --- | --- | --- |
| Left-to-right order | `monitor =` positions | `outputs.order` | done | yes |
| Primary output | — | `outputs.primary` | done | — |
| Resolution and refresh | `monitor = NAME, 2560x1440@144, …` | `outputs.monitors[NAME].mode` | done | yes |
| Position | `monitor = …, 0x0, …` | `outputs.monitors[NAME].position` | done | yes |
| Scale | `monitor = …, 1.25` | `outputs.monitors[NAME].scale` | done | yes |
| Disable an output | `monitor = NAME, disable` | `outputs.monitors[NAME].enabled` | done | yes |
| Transform / rotation | `transform, N` | `outputs.monitors[NAME].transform` | done | yes |
| Match by description | `desc:…` | `outputs.monitors["desc:…"]` | done (untested on hardware) | yes |
| Variable refresh rate | `vrr` | `outputs.monitors[NAME].vrr` | done (untested on hardware) | yes |
| 10-bit color, mirroring | `bitdepth`, `mirror` | — | missing | no |

### Colors and wallpaper

| Setting | Source | shaoDe setting | Status | Importer |
| --- | --- | --- | --- | --- |
| Background color | wallbash, pywal background | `appearance.background` | done | yes |
| Accent / panel / text colors | wallbash, pywal, waybar `@define-color` | `shell.accent`, `panel_color`, `text_color` | done | yes |
| Panel transparency | waybar `background: rgba(…)` | `shell.panel_color` as `#RRGGBBAA` | done | yes |
| Wallpaper | HyDE's current wallpaper (covers swww), pywal, hyprpaper | `shell.wallpaper` | done | yes |
| Font family and size | waybar `font-family`, `font-size` | `shell.font`, `shell.font_size` | done | yes |

### Bar

| Setting | Source | shaoDe setting | Status | Importer |
| --- | --- | --- | --- | --- |
| Height | waybar `height` | `shell.panel_height` | done | yes |
| Top or bottom | waybar `position` | `shell.panel_position` | done | yes |
| Margins (floating bar) | waybar `margin-*` | `shell.panel_margin` | done | yes |
| Corner radius | `window#waybar { border-radius }` | `shell.panel_radius` | done | yes |
| Module layout | waybar `modules-*` | — | won't | — |

### Windows

| Setting | Source | shaoDe setting | Status | Importer |
| --- | --- | --- | --- | --- |
| Outer gap | `general:gaps_out` | `layout.gap_outer` | done | yes |
| Inner gap | `general:gaps_in` (×2: Hyprland adds it on both sides) | `layout.gap_inner` | done | yes |
| Border width | `general:border_size` | `windows.border_width` | done | yes |
| Border colors (active/inactive) | `col.active_border`, `col.inactive_border` | `windows.border_color`, `border_inactive_color` | done | yes |
| Gradient borders | `rgba(…) rgba(…) 45deg` | — (the importer takes the first color) | missing | first color |
| Corner rounding | `decoration:rounding` | — | blocked: needs scenefx or a custom renderer | no |
| Active / inactive opacity | `decoration:active_opacity`, `inactive_opacity` | `windows.opacity`, `inactive_opacity` | done | yes |
| Per-app opacity | `windowrule = opacity A B, class:…` | `windows.rules` (`app_id` regex) | done | yes |
| Blur | `decoration:blur` | — | blocked: needs scenefx or a custom renderer | no |
| Shadows | `decoration:shadow` | — | blocked: needs scenefx or a custom renderer | no |
| Animations | `animation`, `bezier` | — | won't (for now) | — |

### Input

| Setting | Source | shaoDe setting | Status | Importer |
| --- | --- | --- | --- | --- |
| Keyboard layout, options | `input:kb_layout`, `kb_options` | `keyboard.layout`, `options` | done | yes |
| Keyboard variant, keymap file | `input:kb_variant`, `kb_file` | — | missing | no |
| Repeat rate / delay | `input:repeat_rate`, `repeat_delay` | `keyboard.repeat_rate`, `repeat_delay` | done | yes |
| Pointer speed, acceleration | `input:sensitivity`, `accel_profile` | `mouse.speed`, `mouse.acceleration` | done (untested on hardware) | yes |
| Natural scroll, tap-to-click | `input:natural_scroll`, `touchpad:natural_scroll`, `touchpad:tap-to-click`, `touchpad:disable_while_typing` | `mouse.natural_scroll`, `touchpad.*` | done (untested on hardware) | yes |

### Out of scope

Key bindings, dispatchers, `exec`/`exec-once`, window rules other than opacity, layer
rules, gestures, permissions, and anything that talks to `hyprctl`.
