// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No wlroots types cross this boundary. C++ owns configuration and policy. */
enum sh_action {
    SH_NONE,
    SH_HANDLED,
    SH_QUIT,
    SH_CLOSE,
    SH_CYCLE,
    SH_SNAP_LEFT,
    SH_SNAP_RIGHT,
    SH_MAXIMIZE,
    SH_RESTORE,
    SH_TILE,
    SH_RELOAD,
    SH_FULLSCREEN,
    SH_WORKSPACE,         /* argument: workspace number, from 1 */
    SH_MOVE_TO_WORKSPACE, /* argument: workspace number, from 1 */
    SH_WORKSPACE_NEXT,
    SH_WORKSPACE_PREV,
    SH_TOGGLE_TILING,
    SH_TOGGLE_FLOATING,
    SH_LAUNCHER /* asks the shell to toggle its application menu */
};

/* Modifier and edge bits intentionally match wlroots, without importing its headers. */
enum sh_modifier { SH_SHIFT = 1, SH_CTRL = 4, SH_ALT = 8, SH_LOGO = 64 };
enum sh_edge { SH_EDGE_TOP = 1, SH_EDGE_BOTTOM = 2, SH_EDGE_LEFT = 4, SH_EDGE_RIGHT = 8 };

/* Settings for one output, matched by connector name. Zero fields keep the defaults. */
struct sh_monitor {
    char name[32];
    bool enabled;
    int width, height; /* 0: the preferred resolution */
    int refresh;       /* mHz; 0: the fastest at that resolution */
    float scale;       /* 0: 1 */
    bool positioned;   /* x, y are layout coordinates before the primary output shift */
    int x, y;
    int transform; /* enum wl_output_transform, which Hyprland's numbering matches */
};

struct sh_settings {
    float background[4];
    uint32_t mouse_modifier;
    int repeat_rate;
    int repeat_delay;
    int gap_inner; /* between neighbouring windows */
    int gap_outer; /* between windows and the edges of the usable area */
    char keyboard_layout[128];
    char keyboard_options[128];
    bool xwayland; /* read at startup; changing it needs a restart */
    bool tiling;   /* automatic tiling at startup; toggled at runtime afterwards */
    int workspaces;
    /* Outputs named here sit left to right in this order; others follow as they appear.
     * The primary output (or the leftmost, if unnamed) sits at the layout origin. */
    char output_order[8][32];
    int output_count;
    char primary_output[32];
    struct sh_monitor monitors[8];
    int monitor_count;
    /* Drawn outside each window's geometry; placed windows shrink to keep it in their slot. */
    int border_width;
    float border_active[4], border_inactive[4]; /* premultiplied RGBA */
    /* Pointer devices (libinput only). A negative value keeps the device's own default. */
    double pointer_speed; /* -1 to 1; used when pointer_speed_set */
    bool pointer_speed_set;
    int pointer_accel;        /* -1 default, 0 flat, 1 adaptive */
    int mouse_natural_scroll; /* -1, 0, 1 */
    int touchpad_natural_scroll, touchpad_tap, touchpad_dwt;
};

struct sh_callbacks {
    void *userdata;
    const struct sh_settings *(*settings)(void *);
    /* Returns the bound action; *argument receives its numeric argument, if any. */
    enum sh_action (*key)(void *, uint32_t modifiers, uint32_t keysym, int *argument);
    /* Parses a control-socket request into an action; SH_NONE with a message on error. */
    enum sh_action (*command)(void *, const char *request, int *argument, char *error,
                              size_t error_size);
    bool (*reload)(void *);
    void (*startup)(void *);
    void (*child_exited)(void *, int pid);
    /* Opacity for a window with this app ID (may be ""), focused or not. */
    float (*opacity)(void *, const char *app_id, bool active);
};

enum sh_backend_mode { SH_BACKEND_NESTED, SH_BACKEND_HEADLESS, SH_BACKEND_SESSION };
int sh_run(const struct sh_callbacks *callbacks, enum sh_backend_mode mode);

struct sh_rect {
    int x, y, width, height;
};
bool sh_placement(enum sh_action action, struct sh_rect area, int gap, int index, int count,
                  struct sh_rect *result);

/* Automatic tiling in the style of Hyprland's dwindle layout: one binary split tree per output
 * name and workspace. Each split divides its box along the longer side; a new window splits an
 * existing one. Windows are opaque pointers owned by the caller. */
struct sh_tiling;
struct sh_tiling *sh_tiling_create(void);
void sh_tiling_destroy(struct sh_tiling *tiling);
/* Splits `target` if it is tiled, else the window last arranged under the point (with
 * has_point), else the newest window of that tree. With a point inside the split window, the new
 * window takes the half nearer the point; otherwise the right or bottom half. */
void sh_tiling_insert(struct sh_tiling *tiling, const char *output, int workspace, void *window,
                      const void *target, bool has_point, double x, double y);
void sh_tiling_remove(struct sh_tiling *tiling, const void *window);
/* The output name of the window's tree, or NULL when it is not tiled. */
const char *sh_tiling_output(const struct sh_tiling *tiling, const void *window);
typedef void (*sh_tile_place)(void *userdata, void *window, struct sh_rect rect);
void sh_tiling_arrange(struct sh_tiling *tiling, const char *output, int workspace,
                       struct sh_rect area, int gap, sh_tile_place place, void *userdata);
/* Moves the splits beside the window's given edges (enum sh_edge bits) to those edges of
 * `rect`, in the coordinates of the last arrangement. Returns whether anything changed. */
bool sh_tiling_resize(struct sh_tiling *tiling, const void *window, uint32_t edges,
                      struct sh_rect rect);

#ifdef __cplusplus
}
#endif
