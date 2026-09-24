#pragma once
/* Window controls for windows that ask the compositor to decorate them: three small
 * macOS-style dots (close, minimize, fullscreen) on a translucent pill over the window's
 * top-left corner. Dragging the pill moves the window. */
#include <stdbool.h>

struct wlr_buffer;

enum sh_deco_part {
    SH_DECO_NONE,
    SH_DECO_PILL, // the pill between and around the dots: a drag handle
    SH_DECO_CLOSE,
    SH_DECO_MINIMIZE,
    SH_DECO_FULLSCREEN,
};

/* Logical size of the pill and its inset from the window's top-left corner. */
enum { SH_DECO_MARGIN = 8, SH_DECO_WIDTH = 64, SH_DECO_HEIGHT = 22 };

/* Renders the pill at `scale` pixels per logical pixel; `hovered` shows the glyphs in the
 * dots. Returns NULL when out of memory. */
struct wlr_buffer *sh_decoration_render(int scale, bool hovered);

/* The part at pill-local logical coordinates. */
enum sh_deco_part sh_decoration_part_at(double x, double y);
