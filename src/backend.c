/* Derived from wlroots TinyWL 0.20.2; see vendor/tinywl/LICENSE. */
#define _GNU_SOURCE // accept4
#include "shaode/backend.h"
#include "shaode/decoration.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/config.h>
#if WLR_HAS_SESSION
#include <wlr/backend/session.h>
#endif
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/xcursor.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#if SHAODE_XWM_WAKER
#include <xcb/xfixes.h>
#endif
#endif
#include <xkbcommon/xkbcommon.h>

_Static_assert((unsigned)SH_ALT == (unsigned)WLR_MODIFIER_ALT &&
                   (unsigned)SH_SHIFT == (unsigned)WLR_MODIFIER_SHIFT &&
                   (unsigned)SH_CTRL == (unsigned)WLR_MODIFIER_CTRL &&
                   (unsigned)SH_LOGO == (unsigned)WLR_MODIFIER_LOGO,
               "C++ configuration and wlroots modifier bits must agree");
_Static_assert((unsigned)SH_EDGE_TOP == (unsigned)WLR_EDGE_TOP &&
                   (unsigned)SH_EDGE_BOTTOM == (unsigned)WLR_EDGE_BOTTOM &&
                   (unsigned)SH_EDGE_LEFT == (unsigned)WLR_EDGE_LEFT &&
                   (unsigned)SH_EDGE_RIGHT == (unsigned)WLR_EDGE_RIGHT,
               "tiling and wlroots edge bits must agree");

enum sh_cursor_mode {
    SH_CURSOR_PASSTHROUGH,
    SH_CURSOR_MOVE,
    SH_CURSOR_RESIZE,
};

enum sh_node_kind { SH_NODE_TOPLEVEL, SH_NODE_LAYER };
struct sh_node {
    enum sh_node_kind kind;
    void *owner;
};

struct sh_server {
    const struct sh_callbacks *callbacks;
    bool running;
    uint32_t grab_button;
    struct wlr_scene_tree *backgrounds;
    struct wlr_scene_tree *windows;
    struct wlr_scene_tree *fullscreen;
    struct wlr_scene_tree *unmanaged;
#if WLR_HAS_XWAYLAND
    struct wlr_xwayland *xwayland;
    struct wl_listener xwayland_ready, new_xwayland_surface;
#if SHAODE_XWM_WAKER
    xcb_connection_t *xwm_waker;
    xcb_atom_t waker_atom;
    xcb_window_t xwm_window;
    struct wl_event_source *waker_timer, *waker_input;
#endif
#endif
    int workspace; // current workspace, from 0
    struct sh_tiling *tiling;
    bool tiling_enabled;
    bool grab_retile; // the grabbed window left the tiling to be moved; retile it on drop

    int control_fd;
    struct wl_list subscribers; // control clients receiving state changes
    char control_path[108];
    struct wl_event_source *control_source;
    struct wlr_scene_tree *layer_trees[4];
    struct wl_list layers;
    struct wl_listener new_layer_surface;
    struct sh_layer *focused_layer;
    struct sh_toplevel *focused_toplevel;
    struct wlr_foreign_toplevel_manager_v1 *foreign_manager;
    struct wlr_ext_foreign_toplevel_list_v1 *toplevel_list; // windows offered for screen sharing
    struct wl_listener new_capture_request;

    /* Session lock: `locked` outlives a crashed locker so the screen stays covered. */
    bool locked;
    struct sh_lock *lock;
    struct wlr_scene_tree *lock_tree, *lock_blanks;
    struct wl_listener new_lock;
    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wl_listener new_inhibitor;
    int inhibitors;
    struct wl_display *wl_display;
    struct wlr_backend *backend;
#if WLR_HAS_SESSION
    struct wlr_session *session;
#endif
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_list toplevels;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;

    struct wlr_seat *seat;
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_shape;
    uint32_t shape_edges; // edges of the client's single-edge resize shape, else 0
    uint32_t shown_edges; // edges of the resize cursor currently shown for it
    struct wl_listener pointer_focus_change;
    struct wl_listener request_set_selection, request_set_primary_selection;
    struct wl_listener request_start_drag, start_drag;
    struct wlr_scene_tree *drag_icons; // follows the cursor during drag-and-drop
    struct wl_listener request_activate;
    struct wlr_relative_pointer_manager_v1 *relative_pointer;
    struct wlr_pointer_constraints_v1 *constraints;
    struct wlr_pointer_constraint_v1 *active_constraint; // on the keyboard-focused surface
    struct wl_listener new_constraint, keyboard_focus_change;
    struct wl_list keyboards;
    enum sh_cursor_mode cursor_mode;
    struct sh_toplevel *grabbed_toplevel;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;
    /* Window-control pills: shared buffers (plain, hovered), the window whose pill is
     * hovered or revealed over fullscreen, and a dot pressed but not yet released. */
    struct wlr_buffer *deco_buffers[2];
    struct sh_toplevel *deco_hovered, *deco_revealed, *deco_pressed;
    enum sh_deco_part deco_pressed_part;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;
};

struct sh_output {
    struct wlr_box usable;
    struct wlr_scene_rect *background, *lock_blank;
    bool lock_presented;
    struct wl_list link;
    struct sh_server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

struct sh_toplevel {
    struct sh_node node;
    enum sh_action arrangement;
    bool minimized;
    int workspace;
    struct wlr_foreign_toplevel_handle_v1 *foreign;
    /* Window capture: a private scene holding only this window's surfaces, so sharing one
     * window never shows what overlaps it. */
    struct wlr_ext_foreign_toplevel_handle_v1 *listed;
    struct wlr_scene *capture_scene;
    struct wlr_ext_image_capture_source_v1 *capture_source;
    struct wl_listener title_changed, app_id_changed;
    struct wl_listener foreign_activate, foreign_close, foreign_maximize, foreign_minimize;
    struct wl_listener foreign_fullscreen;
    bool fullscreen;
    struct wlr_box fullscreen_restore;
    struct wl_listener request_minimize;
    struct wlr_box restore_box;
    bool arranged;
    bool tiled;    // in the tiling tree; restore_box keeps its floating geometry
    bool floating; // kept out of the tiling (dialogs, or toggled by the user)
    struct wl_list link;
    struct sh_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel; // NULL for X11 windows
#if WLR_HAS_XWAYLAND
    struct wlr_xwayland_surface *xsurface; // NULL for xdg-shell windows
    bool unmanaged, associated;            // unmanaged: override-redirect menus and tooltips
    struct wl_listener x_associate, x_dissociate, x_configure, x_activate, x_geometry;
    struct wl_listener x_decorations;
#endif
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_buffer *deco; // window controls; NULL when the client decorates itself
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
};

struct sh_layer {
    struct sh_node node;
    struct sh_server *server;
    struct wlr_layer_surface_v1 *surface;
    struct wlr_scene_layer_surface_v1 *scene;
    struct wl_list link;
    struct wl_listener commit, map, unmap, destroy, new_popup;
};

struct sh_lock {
    struct sh_server *server;
    struct wlr_session_lock_v1 *lock;
    bool locked_sent;
    struct wl_listener new_surface, unlock, destroy;
};

struct sh_lock_surface {
    struct sh_server *server;
    struct wlr_session_lock_surface_v1 *surface;
    struct wlr_scene_tree *tree;
    struct wl_listener map, destroy;
};

struct sh_inhibitor {
    struct sh_server *server;
    struct wl_listener destroy;
};

struct sh_popup {
    struct sh_server *server;
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

struct sh_keyboard {
    bool consumed[KEY_MAX + 1];
    struct wl_list link;
    struct sh_server *server;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

static void arrange_layers(struct sh_server *server);
static void arrange_windows(struct sh_server *server, enum sh_action action);
static void begin_interactive(struct sh_toplevel *toplevel, enum sh_cursor_mode mode,
                              uint32_t edges);
static void create_popup(struct sh_server *server, struct wlr_xdg_popup *popup,
                         struct wlr_scene_tree *parent);
static void lock_output_presented(struct sh_output *output);
static void notify_subscribers(struct sh_server *server);
static void refit_fullscreen(struct sh_server *server);
static void reflow_output(struct sh_server *server, struct wlr_output *output);
static void reload_config(struct sh_server *server);
static void reset_cursor_mode(struct sh_server *server);
static void set_fullscreen(struct sh_toplevel *toplevel, bool fullscreen);
static void set_tiling(struct sh_server *server, bool enabled);
static void tile_toplevel(struct sh_toplevel *toplevel, struct wlr_output *output,
                          struct sh_toplevel *target, bool at_cursor);
static struct wlr_output *tiled_output(struct sh_toplevel *toplevel);
static void untile_toplevel(struct sh_toplevel *toplevel, bool restore);
static bool wants_tiling(struct sh_toplevel *toplevel);

static const uint32_t ALL_EDGES = WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;

static void add_listener(struct wl_signal *signal, struct wl_listener *listener,
                         wl_notify_func_t notify) {
    listener->notify = notify;
    wl_signal_add(signal, listener);
}

static const struct sh_settings *server_settings(struct sh_server *server) {
    return server->callbacks->settings(server->callbacks->userdata);
}

static struct wlr_output *first_output(struct sh_server *server) {
    if (wl_list_empty(&server->outputs))
        return NULL;
    struct sh_output *first = wl_container_of(server->outputs.next, first, link);
    return first->wlr_output;
}

/* Window operations shared by xdg-shell and XWayland toplevels. */
static struct wlr_surface *toplevel_surface(struct sh_toplevel *toplevel) {
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface)
        return toplevel->xsurface->surface;
#endif
    return toplevel->xdg_toplevel->base->surface;
}
static bool toplevel_mapped(struct sh_toplevel *toplevel) {
    struct wlr_surface *surface = toplevel_surface(toplevel);
    return toplevel->scene_tree && surface && surface->mapped;
}
static struct wlr_box toplevel_geometry(struct sh_toplevel *toplevel) {
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface)
        return (struct wlr_box){0, 0, toplevel->xsurface->width, toplevel->xsurface->height};
#endif
    return toplevel->xdg_toplevel->base->geometry;
}
static void toplevel_set_activated(struct sh_toplevel *toplevel, bool activated) {
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface) {
        wlr_xwayland_surface_activate(toplevel->xsurface, activated);
        if (activated)
            wlr_xwayland_surface_restack(toplevel->xsurface, NULL, XCB_STACK_MODE_ABOVE);
        return;
    }
#endif
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, activated);
}
/* Positions the window in layout coordinates; X11 clients also learn the position. */
static void toplevel_configure(struct sh_toplevel *toplevel, int x, int y, int width, int height) {
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface) {
        if (width > 0 && height > 0)
            wlr_xwayland_surface_configure(toplevel->xsurface, x, y, width, height);
        return;
    }
#endif
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
}
static void toplevel_configure_box(struct sh_toplevel *toplevel, struct wlr_box box) {
    toplevel_configure(toplevel, box.x, box.y, box.width, box.height);
}
/* The window's position and size, as saved to restore it later. */
static struct wlr_box toplevel_box(struct sh_toplevel *toplevel) {
    struct wlr_box geometry = toplevel_geometry(toplevel);
    return (struct wlr_box){toplevel->scene_tree->node.x, toplevel->scene_tree->node.y,
                            geometry.width, geometry.height};
}
static void toplevel_set_position(struct sh_toplevel *toplevel, int x, int y) {
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface && toplevel->xsurface->width > 0 && toplevel->xsurface->height > 0)
        wlr_xwayland_surface_configure(toplevel->xsurface, x, y, toplevel->xsurface->width,
                                       toplevel->xsurface->height);
#endif
}
/* Tells the client and the taskbar whether the window is maximized, and which edges touch
 * a neighbour or the screen edge. */
static void toplevel_set_states(struct sh_toplevel *toplevel, bool maximized, uint32_t tiled) {
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel->foreign, maximized);
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface) {
        wlr_xwayland_surface_set_maximized(toplevel->xsurface, maximized, maximized);
        return;
    }
#endif
    wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, maximized);
    wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, tiled);
}
static void toplevel_set_fullscreen_state(struct sh_toplevel *toplevel, bool fullscreen) {
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface) {
        wlr_xwayland_surface_set_fullscreen(toplevel->xsurface, fullscreen);
        return;
    }
#endif
    wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, fullscreen);
}
/* Answers a denied client request by repeating the current state. */
static void toplevel_refresh(struct sh_toplevel *toplevel) {
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface) {
        if (toplevel_mapped(toplevel))
            toplevel_set_position(toplevel, toplevel->scene_tree->node.x,
                                  toplevel->scene_tree->node.y);
        return;
    }
#endif
    if (toplevel->xdg_toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}
static void toplevel_close(struct sh_toplevel *toplevel) {
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface) {
        wlr_xwayland_surface_close(toplevel->xsurface);
        return;
    }
#endif
    wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
}
static const char *toplevel_title(struct sh_toplevel *toplevel) {
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface)
        return toplevel->xsurface->title;
#endif
    return toplevel->xdg_toplevel->title;
}
static const char *toplevel_app_id(struct sh_toplevel *toplevel) {
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface)
        return toplevel->xsurface->class;
#endif
    return toplevel->xdg_toplevel->app_id;
}
static bool toplevel_accepts_keyboard(struct sh_toplevel *toplevel) {
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface)
        return wlr_xwayland_surface_icccm_input_model(toplevel->xsurface) !=
               WLR_ICCCM_INPUT_MODEL_NONE;
#endif
    return true;
}
static bool toplevel_visible(struct sh_toplevel *toplevel) {
    return !toplevel->minimized && toplevel->workspace == toplevel->server->workspace;
}

/* Shows only the current workspace's windows; focus is left to the caller. */
static void show_workspace(struct sh_server *server, int workspace) {
    server->workspace = workspace;
    struct sh_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        wlr_scene_node_set_enabled(&toplevel->scene_tree->node, toplevel_visible(toplevel));
    }
    wlr_log(WLR_INFO, "Workspace %d", workspace + 1);
    notify_subscribers(server);
}

static void deactivate_toplevel(struct sh_server *server) {
    if (!server->focused_toplevel)
        return;
    struct sh_toplevel *old = server->focused_toplevel;
    if (old->fullscreen)
        wlr_scene_node_reparent(&old->scene_tree->node, server->windows);
    toplevel_set_activated(old, false);
    if (old->foreign)
        wlr_foreign_toplevel_handle_v1_set_activated(old->foreign, false);
    server->focused_toplevel = NULL;
}

static void focus_toplevel(struct sh_toplevel *toplevel) {
    if (!toplevel || toplevel->server->locked)
        return;
    struct sh_server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;
    if (toplevel->workspace != server->workspace) {
        if (server->grabbed_toplevel)
            reset_cursor_mode(server);
        show_workspace(server, toplevel->workspace);
    }
    deactivate_toplevel(server);
    server->focused_layer = NULL;
    server->focused_toplevel = toplevel;
    bool was_minimized = toplevel->minimized;
    toplevel->minimized = false;
    if (was_minimized && wants_tiling(toplevel))
        tile_toplevel(toplevel, NULL, NULL, false);
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
    // Panels stay reachable once a fullscreen window loses focus.
    wlr_scene_node_reparent(&toplevel->scene_tree->node,
                            toplevel->fullscreen ? server->fullscreen : server->windows);
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);
    toplevel_set_activated(toplevel, true);
    if (toplevel->foreign) {
        wlr_foreign_toplevel_handle_v1_set_minimized(toplevel->foreign, false);
        wlr_foreign_toplevel_handle_v1_set_activated(toplevel->foreign, true);
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard && toplevel_accepts_keyboard(toplevel))
        wlr_seat_keyboard_notify_enter(seat, toplevel_surface(toplevel), keyboard->keycodes,
                                       keyboard->num_keycodes, &keyboard->modifiers);
}

static void focus_previous(struct sh_server *server) {
    if (server->locked)
        return;
    server->focused_layer = NULL;
    struct sh_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel_visible(toplevel)) {
            focus_toplevel(toplevel);
            return;
        }
    }
    deactivate_toplevel(server);
    wlr_seat_keyboard_clear_focus(server->seat);
}

static void focus_layer(struct sh_layer *layer) {
    if (layer->server->locked || layer->surface->current.keyboard_interactive ==
                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
        return;
    struct sh_server *server = layer->server;
    deactivate_toplevel(server);
    server->focused_layer = layer;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    if (keyboard)
        wlr_seat_keyboard_notify_enter(server->seat, layer->surface->surface, keyboard->keycodes,
                                       keyboard->num_keycodes, &keyboard->modifiers);
}

static void minimize_toplevel(struct sh_toplevel *toplevel) {
    toplevel->minimized = true;
    untile_toplevel(toplevel, false);
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_minimized(toplevel->foreign, true);
    if (toplevel->server->focused_toplevel == toplevel)
        focus_previous(toplevel->server);
}

static struct sh_rect usable_area(struct sh_server *server, struct wlr_output *output) {
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, output, &box);
    struct sh_output *candidate;
    wl_list_for_each(candidate, &server->outputs, link) {
        if (candidate->wlr_output == output)
            box = candidate->usable;
    }
    return (struct sh_rect){box.x, box.y, box.width, box.height};
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    struct sh_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);

    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);

    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->wlr_keyboard->modifiers);
}

/* The window keyboard actions apply to: the focused one, else the topmost visible. */
static struct sh_toplevel *current_toplevel(struct sh_server *server) {
    if (server->focused_toplevel)
        return server->focused_toplevel;
    struct sh_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel_visible(toplevel))
            return toplevel;
    }
    return NULL;
}

/* A tiled window moves into the tiling of the same output on its new workspace. */
static void set_toplevel_workspace(struct sh_toplevel *toplevel, int workspace) {
    bool retile = toplevel->tiled;
    struct wlr_output *output = tiled_output(toplevel);
    untile_toplevel(toplevel, false);
    toplevel->workspace = workspace;
    if (retile)
        tile_toplevel(toplevel, output, NULL, false);
}

static void switch_workspace(struct sh_server *server, int workspace) {
    int count = server_settings(server)->workspaces;
    if (workspace < 0 || workspace >= count || workspace == server->workspace)
        return;
    if (server->grabbed_toplevel)
        reset_cursor_mode(server);
    deactivate_toplevel(server);
    show_workspace(server, workspace);
    focus_previous(server);
}

static void move_to_workspace(struct sh_server *server, int workspace) {
    struct sh_toplevel *toplevel = current_toplevel(server);
    int count = server_settings(server)->workspaces;
    if (!toplevel || workspace < 0 || workspace >= count || workspace == toplevel->workspace)
        return;
    if (server->grabbed_toplevel == toplevel)
        reset_cursor_mode(server);
    set_toplevel_workspace(toplevel, workspace);
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    if (server->focused_toplevel == toplevel) {
        deactivate_toplevel(server);
        focus_previous(server);
    }
}

/* Shared by key bindings and the control socket. */
static void run_action(struct sh_server *server, enum sh_action action, int argument) {
    int count = server_settings(server)->workspaces;
    struct sh_toplevel *current = current_toplevel(server);
    switch (action) {
    case SH_NONE:
    case SH_HANDLED:
        break;
    case SH_QUIT:
        wl_display_terminate(server->wl_display);
        break;
    case SH_RELOAD:
        reload_config(server);
        break;
    case SH_CYCLE: {
        // Raise the least recently focused visible window.
        struct sh_toplevel *toplevel;
        wl_list_for_each_reverse(toplevel, &server->toplevels, link) {
            if (toplevel != current && toplevel_visible(toplevel)) {
                focus_toplevel(toplevel);
                break;
            }
        }
        break;
    }
    case SH_FULLSCREEN:
        if (current)
            set_fullscreen(current, !current->fullscreen);
        break;
    case SH_CLOSE:
        if (current)
            toplevel_close(current);
        break;
    case SH_WORKSPACE:
        switch_workspace(server, argument - 1);
        break;
    case SH_MOVE_TO_WORKSPACE:
        move_to_workspace(server, argument - 1);
        break;
    case SH_WORKSPACE_NEXT:
        switch_workspace(server, (server->workspace + 1) % count);
        break;
    case SH_WORKSPACE_PREV:
        switch_workspace(server, (server->workspace + count - 1) % count);
        break;
    case SH_TOGGLE_TILING:
        set_tiling(server, !server->tiling_enabled);
        break;
    case SH_TOGGLE_FLOATING:
        if (current && current->tiled) {
            current->floating = true;
            untile_toplevel(current, true);
        } else if (current) {
            current->floating = false;
            if (wants_tiling(current))
                tile_toplevel(current, NULL, NULL, true);
        }
        break;
    default:
        arrange_windows(server, action);
        break;
    }
}

#if WLR_HAS_SESSION
// Returns the VT a key switches to, or 0. Ctrl+AltGr+Fn counts as Ctrl+Alt+Fn: some keyboards'
// only Alt key is Right Alt, which AltGr layouts turn into Level3 instead of Alt.
static unsigned vt_for_key(uint32_t modifiers, xkb_keysym_t sym) {
    if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12)
        return sym - XKB_KEY_XF86Switch_VT_1 + 1;
    if ((modifiers & WLR_MODIFIER_CTRL) && (modifiers & (WLR_MODIFIER_ALT | WLR_MODIFIER_MOD5)) &&
        sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12)
        return sym - XKB_KEY_F1 + 1;
    return 0;
}
#endif

static bool handle_keybinding(struct sh_server *server, uint32_t modifiers, xkb_keysym_t sym) {
#if WLR_HAS_SESSION
    unsigned vt = vt_for_key(modifiers, sym);
    if (server->session && vt) {
        wlr_session_change_vt(server->session, vt);
        return true;
    }
#endif
    if (server->locked)
        return false; // Every other key belongs to the lock screen.
    int argument = 0;
    enum sh_action action =
        server->callbacks->key(server->callbacks->userdata, modifiers, sym, &argument);
    if (action == SH_NONE)
        return false;
    run_action(server, action, argument);
    return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct sh_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct sh_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8;

    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    wlr_idle_notifier_v1_notify_activity(server->idle_notifier, seat);
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms && !handled; ++i)
            handled = handle_keybinding(server, modifiers, syms[i]);
        if (!handled) {
            xkb_layout_index_t layout =
                xkb_state_key_get_layout(keyboard->wlr_keyboard->xkb_state, keycode);
            const xkb_keysym_t *raw;
            int nraw = xkb_keymap_key_get_syms_by_level(keyboard->wlr_keyboard->keymap, keycode,
                                                        layout, 0, &raw);
            for (int i = 0; i < nraw && !handled; ++i)
                handled = handle_keybinding(server, modifiers, raw[i]);
        }
        if (event->keycode <= KEY_MAX)
            keyboard->consumed[event->keycode] = handled;
    } else if (event->keycode <= KEY_MAX) {
        handled = keyboard->consumed[event->keycode];
        keyboard->consumed[event->keycode] = false;
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    struct sh_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static bool configure_keyboard(struct sh_server *server, struct wlr_keyboard *keyboard) {
    const struct sh_settings *settings = server_settings(server);
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context)
        return false;
    struct xkb_rule_names names = {.layout = settings->keyboard_layout,
                                   .options = settings->keyboard_options};
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_context_unref(context);
    if (!keymap)
        return false;
    bool ok = wlr_keyboard_set_keymap(keyboard, keymap);
    xkb_keymap_unref(keymap);
    wlr_keyboard_set_repeat_info(keyboard, settings->repeat_rate, settings->repeat_delay);
    return ok;
}

static void server_new_keyboard(struct sh_server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

    struct sh_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    if (!configure_keyboard(server, wlr_keyboard)) {
        wlr_log(WLR_ERROR, "Failed to configure keyboard");
        free(keyboard);
        return;
    }

    add_listener(&wlr_keyboard->events.modifiers, &keyboard->modifiers, keyboard_handle_modifiers);
    add_listener(&wlr_keyboard->events.key, &keyboard->key, keyboard_handle_key);
    add_listener(&device->events.destroy, &keyboard->destroy, keyboard_handle_destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

    wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_input(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    default:
        break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards))
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, request_cursor);

    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;

    if (focused_client == event->seat_client) {
        server->shape_edges = 0;
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void set_default_cursor(struct sh_server *server) {
    server->shape_edges = 0;
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, pointer_focus_change);
    struct wlr_seat_pointer_focus_change_event *event = data;
    if (!event->new_surface)
        set_default_cursor(server);
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_set_primary_selection(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;
    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

/* Drag-and-drop (browser tabs, files into chat windows): only from a real button press. */
static void seat_request_start_drag(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, request_start_drag);
    struct wlr_seat_request_start_drag_event *event = data;
    if (!server->locked && server->cursor_mode == SH_CURSOR_PASSTHROUGH &&
        wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial))
        wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
    else
        wlr_data_source_destroy(event->drag->source);
}

static void seat_start_drag(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, start_drag);
    struct wlr_drag *drag = data;
    wlr_scene_node_set_position(&server->drag_icons->node, server->cursor->x, server->cursor->y);
    // The scene helper removes the icon's node when the icon goes away.
    if (drag->icon)
        wlr_scene_drag_icon_create(server->drag_icons, drag->icon);
}

static struct sh_toplevel *toplevel_for_surface(struct sh_server *server,
                                                struct wlr_surface *surface) {
    struct sh_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel_surface(toplevel) == surface)
            return toplevel;
    }
    return NULL;
}

/* xdg-activation: an application asks to be raised, e.g. a browser opening a link from chat.
 * wlroots expires and validates tokens. Tokens made without an input serial are honoured too:
 * a browser handed a link by another process often has nothing better. */
static void request_activate(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, request_activate);
    struct wlr_xdg_activation_v1_request_activate_event *event = data;
    struct sh_toplevel *toplevel = toplevel_for_surface(server, event->surface);
    if (toplevel && toplevel_mapped(toplevel))
        focus_toplevel(toplevel);
}

/* Pointer constraints (games, remote desktops, pointer lock in browsers) apply to the
 * keyboard-focused surface only, and only while the pointer is over it. */
static void set_active_constraint(struct sh_server *server,
                                  struct wlr_pointer_constraint_v1 *constraint) {
    if (server->active_constraint == constraint)
        return;
    if (server->active_constraint)
        wlr_pointer_constraint_v1_send_deactivated(server->active_constraint);
    server->active_constraint = constraint;
    if (constraint)
        wlr_pointer_constraint_v1_send_activated(constraint);
}

static void constraint_destroy(struct wl_listener *listener, void *data) {
    struct wlr_pointer_constraint_v1 *constraint = data;
    struct sh_server *server = constraint->data;
    wl_list_remove(&listener->link);
    free(listener);
    if (server->active_constraint == constraint)
        server->active_constraint = NULL;
}

static void server_new_constraint(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;
    struct wl_listener *destroy = calloc(1, sizeof(*destroy));
    if (!destroy)
        return;
    constraint->data = server;
    add_listener(&constraint->events.destroy, destroy, constraint_destroy);
    if (constraint->surface == server->seat->keyboard_state.focused_surface)
        set_active_constraint(server, constraint);
}

static void seat_keyboard_focus_change(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, keyboard_focus_change);
    struct wlr_seat_keyboard_focus_change_event *event = data;
    set_active_constraint(server, event->new_surface
                                      ? wlr_pointer_constraints_v1_constraint_for_surface(
                                            server->constraints, event->new_surface, server->seat)
                                      : NULL);
}

static struct sh_node *desktop_node_at(struct sh_server *server, double lx, double ly,
                                       struct wlr_surface **surface, double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }

    *surface = scene_surface->surface;

    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    return tree ? tree->node.data : NULL;
}

static struct sh_toplevel *desktop_toplevel_at(struct sh_server *server, double x, double y,
                                               struct wlr_surface **surface, double *sx,
                                               double *sy) {
    struct sh_node *node = desktop_node_at(server, x, y, surface, sx, sy);
    return node && node->kind == SH_NODE_TOPLEVEL ? node->owner : NULL;
}

/* Clients report exact corners only in a few pixels; a single-edge grab near the end of that
 * edge is almost always meant as a corner resize. */
static uint32_t corner_edges(struct sh_toplevel *toplevel, uint32_t edges) {
    struct wlr_cursor *cursor = toplevel->server->cursor;
    struct wlr_box geo_box = toplevel_geometry(toplevel);
    double left = toplevel->scene_tree->node.x + geo_box.x;
    double top = toplevel->scene_tree->node.y + geo_box.y;
    double margin_x = geo_box.width / 4.0 < 32 ? geo_box.width / 4.0 : 32;
    double margin_y = geo_box.height / 4.0 < 32 ? geo_box.height / 4.0 : 32;
    if ((edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) == 0) {
        if (cursor->x < left + margin_x)
            edges |= WLR_EDGE_LEFT;
        else if (cursor->x > left + geo_box.width - margin_x)
            edges |= WLR_EDGE_RIGHT;
    }
    if ((edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) == 0) {
        if (cursor->y < top + margin_y)
            edges |= WLR_EDGE_TOP;
        else if (cursor->y > top + geo_box.height - margin_y)
            edges |= WLR_EDGE_BOTTOM;
    }
    return edges;
}

/* Show a corner cursor wherever an edge grab would become a corner resize, so the pointer
 * matches what dragging will do. */
static void update_resize_cursor(struct sh_server *server, struct sh_toplevel *toplevel) {
    if (!server->shape_edges || !toplevel)
        return;
    uint32_t edges = corner_edges(toplevel, server->shape_edges);
    if (edges == server->shown_edges)
        return;
    server->shown_edges = edges;
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, wlr_xcursor_get_resize_name(edges));
}

static uint32_t shape_edges(enum wp_cursor_shape_device_v1_shape shape) {
    switch (shape) {
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE:
        return WLR_EDGE_TOP;
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE:
        return WLR_EDGE_BOTTOM;
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE:
        return WLR_EDGE_LEFT;
    case WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE:
        return WLR_EDGE_RIGHT;
    default:
        return 0;
    }
}

static void cursor_request_set_shape(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, request_set_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER ||
        server->seat->pointer_state.focused_client != event->seat_client)
        return;
    server->shape_edges = shape_edges(event->shape);
    server->shown_edges = 0;
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                           wlr_cursor_shape_v1_name(event->shape));
    if (server->cursor_mode == SH_CURSOR_PASSTHROUGH) {
        struct wlr_surface *surface;
        double sx, sy;
        update_resize_cursor(server, desktop_toplevel_at(server, server->cursor->x,
                                                         server->cursor->y, &surface, &sx, &sy));
    }
}

static void reset_cursor_mode(struct sh_server *server) {
    server->cursor_mode = SH_CURSOR_PASSTHROUGH;
    server->grabbed_toplevel = NULL;
    server->grab_retile = false;
}

/* Dropping a window dragged out of the tiling splits the tile under the pointer. */
static void finish_grab(struct sh_server *server) {
    struct sh_toplevel *toplevel = server->grabbed_toplevel;
    if (server->grab_retile && toplevel && wants_tiling(toplevel))
        tile_toplevel(toplevel,
                      wlr_output_layout_output_at(server->output_layout, server->cursor->x,
                                                  server->cursor->y),
                      NULL, true);
    server->grab_retile = false;
}

static void process_cursor_move(struct sh_server *server) {
    struct sh_toplevel *toplevel = server->grabbed_toplevel;
    toplevel_set_position(toplevel, server->cursor->x - server->grab_x,
                          server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct sh_server *server) {
    struct sh_toplevel *toplevel = server->grabbed_toplevel;
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x;
    int new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y;
    int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    if (toplevel->tiled) {
        // Resizing a tile moves the splits beside the dragged edges instead.
        struct sh_rect rect = {new_left, new_top, new_right - new_left, new_bottom - new_top};
        struct wlr_output *output = tiled_output(toplevel);
        if (sh_tiling_resize(server->tiling, toplevel, server->resize_edges, rect) && output)
            reflow_output(server, output);
        return;
    }
    struct wlr_box geo_box = toplevel_geometry(toplevel);
    toplevel_configure(toplevel, new_left - geo_box.x, new_top - geo_box.y, new_right - new_left,
                       new_bottom - new_top);
}

static bool wants_decoration(struct sh_toplevel *toplevel) {
#if WLR_HAS_XWAYLAND
    // X11 windows that leave decorations to the window manager (Spotify, for one).
    return toplevel->xsurface && !toplevel->unmanaged &&
           toplevel->xsurface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
#else
    return false;
#endif
}

static struct wlr_buffer *deco_buffer(struct sh_server *server, bool hovered) {
    if (!server->deco_buffers[hovered]) {
        float scale = 1;
        struct sh_output *output;
        wl_list_for_each(output, &server->outputs, link) {
            if (output->wlr_output->scale > scale)
                scale = output->wlr_output->scale;
        }
        server->deco_buffers[hovered] = sh_decoration_render((int)ceilf(scale), hovered);
    }
    return server->deco_buffers[hovered];
}

/* Over a fullscreen window the pill hides until the pointer nears its corner. */
static bool in_deco_corner(struct sh_toplevel *toplevel, double x, double y) {
    double left = toplevel->scene_tree->node.x, top = toplevel->scene_tree->node.y;
    return x >= left && y >= top && x < left + 2 * SH_DECO_MARGIN + SH_DECO_WIDTH &&
           y < top + 2 * SH_DECO_MARGIN + SH_DECO_HEIGHT;
}

/* Adds, removes, or updates a window's pill to match what it asks for and its state. */
static void refresh_decoration(struct sh_toplevel *toplevel) {
    struct sh_server *server = toplevel->server;
    if (!toplevel->scene_tree)
        return;
    if (!wants_decoration(toplevel)) {
        if (toplevel->deco)
            wlr_scene_node_destroy(&toplevel->deco->node);
        toplevel->deco = NULL;
        return;
    }
    bool hovered = server->deco_hovered == toplevel;
    struct wlr_buffer *buffer = deco_buffer(server, hovered);
    if (!buffer)
        return;
    if (!toplevel->deco) {
        toplevel->deco = wlr_scene_buffer_create(toplevel->scene_tree, buffer);
        if (!toplevel->deco)
            return;
        wlr_scene_buffer_set_dest_size(toplevel->deco, SH_DECO_WIDTH, SH_DECO_HEIGHT);
    } else {
        wlr_scene_buffer_set_buffer(toplevel->deco, buffer);
    }
    struct wlr_box geometry = toplevel_geometry(toplevel);
    wlr_scene_node_set_position(&toplevel->deco->node, geometry.x + SH_DECO_MARGIN,
                                geometry.y + SH_DECO_MARGIN);
    wlr_scene_node_raise_to_top(&toplevel->deco->node);
    wlr_scene_node_set_enabled(&toplevel->deco->node,
                               !toplevel->fullscreen || server->deco_revealed == toplevel);
}

static void forget_decoration(struct sh_toplevel *toplevel) {
    struct sh_server *server = toplevel->server;
    if (server->deco_hovered == toplevel)
        server->deco_hovered = NULL;
    if (server->deco_revealed == toplevel)
        server->deco_revealed = NULL;
    if (server->deco_pressed == toplevel)
        server->deco_pressed = NULL;
    toplevel->deco = NULL; // destroyed with the scene tree
}

/* The window whose pill is at (x, y), and which part of it. */
static struct sh_toplevel *deco_at(struct sh_server *server, double x, double y,
                                   enum sh_deco_part *part) {
    double sx, sy;
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, x, y, &sx, &sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER || !node->parent)
        return NULL;
    struct sh_node *owner = node->parent->node.data;
    if (!owner || owner->kind != SH_NODE_TOPLEVEL)
        return NULL;
    struct sh_toplevel *toplevel = owner->owner;
    if (!toplevel->deco || &toplevel->deco->node != node)
        return NULL;
    *part = sh_decoration_part_at(sx, sy);
    return *part == SH_DECO_NONE ? NULL : toplevel;
}

static void set_deco_hovered(struct sh_server *server, struct sh_toplevel *toplevel) {
    struct sh_toplevel *old = server->deco_hovered;
    if (old == toplevel)
        return;
    server->deco_hovered = toplevel;
    if (old)
        refresh_decoration(old);
    if (toplevel)
        refresh_decoration(toplevel);
}

static void deco_activate(struct sh_toplevel *toplevel, enum sh_deco_part part) {
    switch (part) {
    case SH_DECO_CLOSE:
        toplevel_close(toplevel);
        break;
    case SH_DECO_MINIMIZE:
        minimize_toplevel(toplevel);
        break;
    case SH_DECO_FULLSCREEN:
        set_fullscreen(toplevel, !toplevel->fullscreen);
        break;
    default:
        break;
    }
}

static void process_cursor_motion(struct sh_server *server, uint32_t time) {
    if (server->seat->drag)
        wlr_scene_node_set_position(&server->drag_icons->node, server->cursor->x,
                                    server->cursor->y);
    if (server->cursor_mode == SH_CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    }
    if (server->cursor_mode == SH_CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct sh_toplevel *revealed = server->deco_revealed;
    if (revealed && !in_deco_corner(revealed, server->cursor->x, server->cursor->y)) {
        server->deco_revealed = NULL;
        refresh_decoration(revealed);
    }
    enum sh_deco_part part;
    struct sh_toplevel *decorated = deco_at(server, server->cursor->x, server->cursor->y, &part);
    set_deco_hovered(server, decorated);
    if (decorated) {
        set_default_cursor(server);
        wlr_seat_pointer_clear_focus(seat);
        return;
    }
    struct wlr_surface *surface = NULL;
    struct sh_toplevel *toplevel =
        desktop_toplevel_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (toplevel && toplevel->deco && toplevel->fullscreen && !server->deco_revealed &&
        in_deco_corner(toplevel, server->cursor->x, server->cursor->y)) {
        server->deco_revealed = toplevel;
        refresh_decoration(toplevel);
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
        update_resize_cursor(server, toplevel);
    } else {
        set_default_cursor(server);
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
    wlr_relative_pointer_manager_v1_send_relative_motion(
        server->relative_pointer, server->seat, (uint64_t)event->time_msec * 1000, event->delta_x,
        event->delta_y, event->unaccel_dx, event->unaccel_dy);
    double dx = event->delta_x, dy = event->delta_y;
    struct wlr_pointer_constraint_v1 *constraint = server->active_constraint;
    if (constraint && server->cursor_mode == SH_CURSOR_PASSTHROUGH &&
        server->seat->pointer_state.focused_surface == constraint->surface) {
        if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
            return; // The client only wants the relative motion sent above.
        double sx = server->seat->pointer_state.sx, sy = server->seat->pointer_state.sy;
        double confined_x, confined_y;
        if (wlr_region_confine(&constraint->region, sx, sy, sx + dx, sy + dy, &confined_x,
                               &confined_y)) {
            dx = confined_x - sx;
            dy = confined_y - sy;
        }
    }
    wlr_cursor_move(server->cursor, &event->pointer->base, dx, dy);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
    if (server->deco_pressed && event->button == BTN_LEFT &&
        event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        // A dot acts on release, and only if the pointer is still on it.
        struct sh_toplevel *pressed = server->deco_pressed;
        server->deco_pressed = NULL;
        enum sh_deco_part part;
        if (deco_at(server, server->cursor->x, server->cursor->y, &part) == pressed &&
            part == server->deco_pressed_part)
            deco_activate(pressed, part);
        process_cursor_motion(server, event->time_msec);
        return;
    }
    if (server->grab_button == event->button && event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        server->grab_button = 0;
        finish_grab(server);
        reset_cursor_mode(server);
        process_cursor_motion(server, event->time_msec);
        return;
    }
    enum sh_deco_part part;
    struct sh_toplevel *decorated =
        event->state == WL_POINTER_BUTTON_STATE_PRESSED && !server->locked && !server->deco_pressed
            ? deco_at(server, server->cursor->x, server->cursor->y, &part)
            : NULL;
    if (decorated) {
        focus_toplevel(decorated);
        if (event->button != BTN_LEFT)
            return;
        if (part == SH_DECO_PILL) {
            server->grab_button = BTN_LEFT;
            begin_interactive(decorated, SH_CURSOR_MOVE, 0);
        } else {
            server->deco_pressed = decorated;
            server->deco_pressed_part = part;
        }
        return;
    }
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct sh_node *node =
            desktop_node_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
        struct sh_toplevel *toplevel = node && node->kind == SH_NODE_TOPLEVEL ? node->owner : NULL;
        if (server->locked) {
            node = NULL; // Only lock surfaces, which have no desktop node, are reachable.
            toplevel = NULL;
        }
        if (toplevel)
            focus_toplevel(toplevel);
        else if (node && node->kind == SH_NODE_LAYER)
            focus_layer(node->owner);
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
        uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
        const struct sh_settings *settings = server_settings(server);
        if (toplevel && (mods & settings->mouse_modifier) &&
            (event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
            server->grab_button = event->button;
            uint32_t edges = WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
            if (toplevel->tiled) {
                // A tile's outer edges cannot move, so resize from the corner nearest the pointer.
                struct wlr_box geometry = toplevel_geometry(toplevel);
                double center_x = toplevel->scene_tree->node.x + geometry.x + geometry.width / 2.0;
                double center_y = toplevel->scene_tree->node.y + geometry.y + geometry.height / 2.0;
                edges = (server->cursor->x < center_x ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
                        (server->cursor->y < center_y ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
            }
            begin_interactive(toplevel,
                              event->button == BTN_LEFT ? SH_CURSOR_MOVE : SH_CURSOR_RESIZE, edges);
            return;
        }
    }
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        finish_grab(server);
        reset_cursor_mode(server);
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta,
                                 event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, cursor_frame);

    wlr_seat_pointer_notify_frame(server->seat);
}

static void output_frame(struct wl_listener *listener, void *data) {
    struct sh_output *output = wl_container_of(listener, output, frame);
    struct wlr_scene *scene = output->server->scene;

    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);

    wlr_scene_output_commit(scene_output, NULL);
    lock_output_presented(output);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void update_backgrounds(struct sh_server *server) {
    const struct sh_settings *settings = server_settings(server);
    struct sh_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
        wlr_scene_node_set_position(&output->background->node, box.x, box.y);
        wlr_scene_rect_set_size(output->background, box.width, box.height);
        wlr_scene_rect_set_color(output->background, settings->background);
        wlr_scene_node_set_position(&output->lock_blank->node, box.x, box.y);
        wlr_scene_rect_set_size(output->lock_blank, box.width, box.height);
    }
}

/* ext-session-lock-v1: an opaque cover hides the desktop from the moment a lock
 * starts; lock surfaces sit above it, and `locked` is sent once every output has
 * presented a covered frame. */
static void send_locked_if_presented(struct sh_server *server) {
    struct sh_lock *lock = server->lock;
    if (!lock || lock->locked_sent)
        return;
    struct sh_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (!output->lock_presented)
            return;
    }
    lock->locked_sent = true;
    wlr_session_lock_v1_send_locked(lock->lock);
}

static void lock_output_presented(struct sh_output *output) {
    if (!output->server->locked || output->lock_presented)
        return;
    output->lock_presented = true;
    send_locked_if_presented(output->server);
}

static void lock_surface_map(struct wl_listener *listener, void *data) {
    struct sh_lock_surface *lock_surface = wl_container_of(listener, lock_surface, map);
    struct sh_server *server = lock_surface->server;
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    if (server->locked && keyboard && !server->seat->keyboard_state.focused_surface)
        wlr_seat_keyboard_notify_enter(server->seat, lock_surface->surface->surface,
                                       keyboard->keycodes, keyboard->num_keycodes,
                                       &keyboard->modifiers);
    process_cursor_motion(server, 0);
}

static void lock_surface_destroy(struct wl_listener *listener, void *data) {
    struct sh_lock_surface *lock_surface = wl_container_of(listener, lock_surface, destroy);
    struct sh_server *server = lock_surface->server;
    if (server->seat->keyboard_state.focused_surface == lock_surface->surface->surface) {
        wlr_seat_keyboard_clear_focus(server->seat);
        // Hand the keyboard to another lock surface, if one remains.
        struct wlr_session_lock_surface_v1 *other;
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
        if (server->lock && keyboard) {
            wl_list_for_each(other, &server->lock->lock->surfaces, link) {
                if (other != lock_surface->surface && other->surface->mapped) {
                    wlr_seat_keyboard_notify_enter(server->seat, other->surface, keyboard->keycodes,
                                                   keyboard->num_keycodes, &keyboard->modifiers);
                    break;
                }
            }
        }
    }
    wl_list_remove(&lock_surface->map.link);
    wl_list_remove(&lock_surface->destroy.link);
    wlr_scene_node_destroy(&lock_surface->tree->node);
    free(lock_surface);
}

static void lock_new_surface(struct wl_listener *listener, void *data) {
    struct sh_lock *lock = wl_container_of(listener, lock, new_surface);
    struct sh_server *server = lock->server;
    struct wlr_session_lock_surface_v1 *surface = data;
    struct sh_lock_surface *lock_surface = calloc(1, sizeof(*lock_surface));
    if (!lock_surface)
        return;
    lock_surface->server = server;
    lock_surface->surface = surface;
    lock_surface->tree = wlr_scene_subsurface_tree_create(server->lock_tree, surface->surface);
    if (!lock_surface->tree) {
        free(lock_surface);
        return;
    }
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, surface->output, &box);
    wlr_scene_node_set_position(&lock_surface->tree->node, box.x, box.y);
    wlr_session_lock_surface_v1_configure(surface, box.width, box.height);
    add_listener(&surface->surface->events.map, &lock_surface->map, lock_surface_map);
    add_listener(&surface->events.destroy, &lock_surface->destroy, lock_surface_destroy);
}

static void lock_unlock(struct wl_listener *listener, void *data) {
    struct sh_lock *lock = wl_container_of(listener, lock, unlock);
    struct sh_server *server = lock->server;
    server->locked = false;
    wlr_scene_node_set_enabled(&server->lock_tree->node, false);
    wlr_seat_keyboard_clear_focus(server->seat);
    if (server->focused_toplevel && !server->focused_toplevel->minimized)
        focus_toplevel(server->focused_toplevel);
    else
        focus_previous(server);
    process_cursor_motion(server, 0);
    wlr_log(WLR_INFO, "Session unlocked");
}

static void lock_destroy(struct wl_listener *listener, void *data) {
    struct sh_lock *lock = wl_container_of(listener, lock, destroy);
    struct sh_server *server = lock->server;
    if (server->locked)
        wlr_log(WLR_ERROR, "Lock client vanished; the session stays locked until a new lock");
    wl_list_remove(&lock->new_surface.link);
    wl_list_remove(&lock->unlock.link);
    wl_list_remove(&lock->destroy.link);
    server->lock = NULL;
    free(lock);
}

static void server_new_lock(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_lock);
    struct wlr_session_lock_v1 *wlr_lock = data;
    struct sh_lock *lock = server->lock ? NULL : calloc(1, sizeof(*lock));
    if (!lock) {
        wlr_session_lock_v1_destroy(wlr_lock); // Another locker is active: `finished`.
        return;
    }
    lock->server = server;
    lock->lock = wlr_lock;
    add_listener(&wlr_lock->events.new_surface, &lock->new_surface, lock_new_surface);
    add_listener(&wlr_lock->events.unlock, &lock->unlock, lock_unlock);
    add_listener(&wlr_lock->events.destroy, &lock->destroy, lock_destroy);
    server->lock = lock;
    bool relock = server->locked;
    server->locked = true;
    if (server->grabbed_toplevel)
        reset_cursor_mode(server);
    wlr_seat_keyboard_clear_focus(server->seat);
    wlr_seat_pointer_clear_focus(server->seat);
    wlr_scene_node_set_enabled(&server->lock_tree->node, true);
    wlr_log(WLR_INFO, "Session locked");
    struct sh_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        // A replacement locker for an abandoned lock finds the cover already shown.
        output->lock_presented = relock;
        wlr_output_schedule_frame(output->wlr_output);
    }
    send_locked_if_presented(server);
}

static void inhibitor_destroy(struct wl_listener *listener, void *data) {
    struct sh_inhibitor *inhibitor = wl_container_of(listener, inhibitor, destroy);
    struct sh_server *server = inhibitor->server;
    wl_list_remove(&inhibitor->destroy.link);
    free(inhibitor);
    wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, --server->inhibitors > 0);
}

/* Video players and games keep the session awake while any inhibitor exists. */
static void server_new_inhibitor(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_inhibitor);
    struct wlr_idle_inhibitor_v1 *wlr_inhibitor = data;
    struct sh_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
    if (!inhibitor)
        return;
    inhibitor->server = server;
    add_listener(&wlr_inhibitor->events.destroy, &inhibitor->destroy, inhibitor_destroy);
    wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, ++server->inhibitors > 0);
}

static bool output_named(const struct sh_output *output, const char *name) {
    return strcmp(output->wlr_output->name, name) == 0;
}

static bool output_listed(const struct sh_settings *settings, const struct sh_output *output) {
    for (int i = 0; i < settings->output_count; ++i) {
        if (output_named(output, settings->output_order[i]))
            return true;
    }
    return false;
}

/* Lays outputs side by side, top-aligned: configured order first, then the rest in the order
 * they appeared. Shifts the row so the primary output starts at x = 0, where the cursor begins. */
static void arrange_outputs(struct sh_server *server) {
    const struct sh_settings *settings = server_settings(server);
    int origin = 0;
    for (int pass = 0; pass < 2; ++pass) {
        int x = 0;
        struct sh_output *output;
        for (int i = 0; i <= settings->output_count; ++i) {
            wl_list_for_each_reverse(output, &server->outputs, link) {
                if (i < settings->output_count ? !output_named(output, settings->output_order[i])
                                               : output_listed(settings, output))
                    continue;
                if (pass == 0 && output_named(output, settings->primary_output))
                    origin = x;
                if (pass == 1)
                    wlr_output_layout_add(server->output_layout, output->wlr_output, x - origin, 0);
                int width, height;
                wlr_output_effective_resolution(output->wlr_output, &width, &height);
                x += width;
            }
        }
    }
    update_backgrounds(server);
    arrange_layers(server);
    refit_fullscreen(server);
}

static void reload_config(struct sh_server *server) {
    if (!server->callbacks->reload(server->callbacks->userdata))
        return;
    struct sh_keyboard *keyboard;
    wl_list_for_each(keyboard, &server->keyboards, link) {
        if (!configure_keyboard(server, keyboard->wlr_keyboard))
            wlr_log(WLR_ERROR, "Could not apply reloaded keymap");
    }
    arrange_outputs(server);
    int count = server_settings(server)->workspaces;
    struct sh_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel->workspace >= count)
            set_toplevel_workspace(toplevel, count - 1);
    }
    if (server->workspace >= count) {
        deactivate_toplevel(server);
        show_workspace(server, count - 1);
        focus_previous(server);
    }
    // The gap may have changed.
    struct sh_output *output;
    wl_list_for_each(output, &server->outputs, link) reflow_output(server, output->wlr_output);
}

static void output_request_state(struct wl_listener *listener, void *data) {
    struct sh_output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    if (wlr_output_commit_state(output->wlr_output, event->state))
        arrange_outputs(output->server);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    struct sh_output *output = wl_container_of(listener, output, destroy);

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    struct sh_layer *layer, *temporary;
    wl_list_for_each_safe(layer, temporary, &output->server->layers, link) {
        if (layer->surface->output == output->wlr_output)
            wlr_layer_surface_v1_destroy(layer->surface);
    }
    wlr_scene_node_destroy(&output->background->node);
    wlr_scene_node_destroy(&output->lock_blank->node);
    struct sh_server *server = output->server;
    // Closing the host window ends a nested session. A standalone session loses every output
    // on VT switch (wlroots recreates them on return) or when the last monitor is unplugged.
    bool standalone = false;
#if WLR_HAS_SESSION
    standalone = server->session != NULL;
#endif
    if (server->running && !standalone && wl_list_empty(&server->outputs))
        wl_display_terminate(server->wl_display);
    free(output);
    if (server->running)
        arrange_outputs(server);
    send_locked_if_presented(server);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    // Monitors often mark a 60 Hz mode as preferred; keep that resolution at its fastest refresh.
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        struct wlr_output_mode *fastest = mode, *candidate;
        wl_list_for_each(candidate, &wlr_output->modes, link) {
            if (candidate->width == mode->width && candidate->height == mode->height &&
                candidate->refresh > fastest->refresh)
                fastest = candidate;
        }
        wlr_output_state_set_mode(&state, fastest);
        if (fastest != mode && !wlr_output_test_state(wlr_output, &state))
            wlr_output_state_set_mode(&state, mode);
    }

    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct sh_output *output = calloc(1, sizeof(*output));
    output->wlr_output = wlr_output;
    output->server = server;
    output->background =
        wlr_scene_rect_create(server->backgrounds, 1, 1, server_settings(server)->background);
    static const float lock_color[4] = {0, 0, 0, 1};
    // lock_presented starts false: an output added while locked must not show the desktop.
    output->lock_blank = wlr_scene_rect_create(server->lock_blanks, 1, 1, lock_color);
    add_listener(&wlr_output->events.frame, &output->frame, output_frame);
    add_listener(&wlr_output->events.request_state, &output->request_state, output_request_state);
    add_listener(&wlr_output->events.destroy, &output->destroy, output_destroy);

    wl_list_insert(&server->outputs, &output->link);

    struct wlr_output_layout_output *l_output =
        wlr_output_layout_add(server->output_layout, wlr_output, 0, 0);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
    if (wlr_output_is_wl(wlr_output))
        wlr_wl_output_set_title(wlr_output, "shaoDe — nested desktop");
    arrange_outputs(server);
}

/* Preserve the original floating rectangle across repeated snap operations. */
static void place_toplevel(struct sh_toplevel *toplevel, enum sh_action action,
                           struct sh_rect target) {
    if (!toplevel->arranged)
        toplevel->restore_box = toplevel_box(toplevel);
    toplevel->arranged = true;
    toplevel->arrangement = action;
    toplevel_set_states(toplevel, action == SH_MAXIMIZE, action == SH_MAXIMIZE ? 0 : ALL_EDGES);
    toplevel_configure(toplevel, target.x, target.y, target.width, target.height);
}

static void restore_toplevel(struct sh_toplevel *toplevel) {
    if (!toplevel->arranged)
        return;
    toplevel->arranged = false;
    toplevel_set_states(toplevel, false, 0);
    toplevel_configure_box(toplevel, toplevel->restore_box);
}

static struct wlr_output *toplevel_output(struct sh_toplevel *toplevel) {
    struct sh_server *server = toplevel->server;
    struct wlr_output *output = wlr_output_layout_output_at(
        server->output_layout, toplevel->scene_tree->node.x, toplevel->scene_tree->node.y);
    return output ? output : first_output(server);
}

static void place_maximized(struct sh_toplevel *toplevel) {
    struct wlr_output *output = toplevel_output(toplevel);
    if (output)
        place_toplevel(toplevel, SH_MAXIMIZE, usable_area(toplevel->server, output));
}

/* Windows taking part in the one-shot grid arrangement of `output`. */
static bool in_grid(struct sh_toplevel *toplevel, struct wlr_output *output) {
    return toplevel_visible(toplevel) && !toplevel->fullscreen &&
           toplevel_output(toplevel) == output;
}

static void arrange_windows(struct sh_server *server, enum sh_action action) {
    struct sh_toplevel *focused = current_toplevel(server);
    if (!focused)
        return;
    if (action == SH_TILE && server->tiling_enabled)
        return; // Already tiled automatically.
    if (focused->tiled && action == SH_RESTORE)
        return;
    if (focused->fullscreen)
        set_fullscreen(focused, false);
    if (action == SH_RESTORE) {
        restore_toplevel(focused);
        return;
    }
    // A tiled window placed by hand floats from then on.
    if (focused->tiled) {
        focused->floating = true;
        untile_toplevel(focused, false);
    }
    struct wlr_output *output = toplevel_output(focused);
    if (!output)
        return;
    struct sh_rect area = usable_area(server, output), target;
    int gap = server_settings(server)->gap;
    if (action != SH_TILE) {
        if (sh_placement(action, area, gap, 0, 1, &target))
            place_toplevel(focused, action, target);
        return;
    }
    int count = 0, index = 0;
    struct sh_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) count += in_grid(toplevel, output);
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (in_grid(toplevel, output) && sh_placement(action, area, gap, index++, count, &target))
            place_toplevel(toplevel, action, target);
    }
}

static void place_tiled(void *data, void *window, struct sh_rect rect) {
    struct sh_toplevel *toplevel = window;
    if (toplevel->fullscreen)
        return; // It returns to its tile when it leaves fullscreen.
    toplevel_set_states(toplevel, false, ALL_EDGES);
    toplevel_configure(toplevel, rect.x, rect.y, rect.width, rect.height);
}

/* Snapped, maximized, or grid-arranged windows that follow changes to the usable area. */
static bool reflows(struct sh_toplevel *toplevel, int workspace, struct wlr_output *output) {
    return toplevel->workspace == workspace && toplevel->arranged && !toplevel->minimized &&
           !toplevel->fullscreen && toplevel_output(toplevel) == output;
}

static void reflow_output(struct sh_server *server, struct wlr_output *output) {
    struct sh_rect area = usable_area(server, output), target;
    const struct sh_settings *settings = server_settings(server);
    for (int workspace = 0; workspace < settings->workspaces; ++workspace) {
        int count = 0, index = 0;
        struct sh_toplevel *toplevel;
        wl_list_for_each(toplevel, &server->toplevels, link) {
            count += reflows(toplevel, workspace, output) && toplevel->arrangement == SH_TILE;
        }
        wl_list_for_each(toplevel, &server->toplevels, link) {
            if (!reflows(toplevel, workspace, output))
                continue;
            enum sh_action action = toplevel->arrangement;
            if (sh_placement(action, area, settings->gap, action == SH_TILE ? index++ : 0,
                             action == SH_TILE ? count : 1, &target))
                place_toplevel(toplevel, action, target);
        }
        sh_tiling_arrange(server->tiling, output->name, workspace, area, settings->gap, place_tiled,
                          NULL);
    }
}

static struct wlr_output *find_output(struct sh_server *server, const char *name) {
    struct sh_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output_named(output, name))
            return output->wlr_output;
    }
    return NULL;
}

static struct wlr_output *tiled_output(struct sh_toplevel *toplevel) {
    const char *name = sh_tiling_output(toplevel->server->tiling, toplevel);
    return name ? find_output(toplevel->server, name) : NULL;
}

static bool wants_tiling(struct sh_toplevel *toplevel) {
    return toplevel->server->tiling_enabled && !toplevel->tiled && !toplevel->floating &&
           !toplevel->minimized;
}

/* Dialogs and fixed-size windows float, as in Hyprland. */
static bool toplevel_is_dialog(struct sh_toplevel *toplevel) {
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface)
        return toplevel->xsurface->parent || toplevel->xsurface->modal;
#endif
    const struct wlr_xdg_toplevel_state *state = &toplevel->xdg_toplevel->current;
    return toplevel->xdg_toplevel->parent ||
           (state->min_width > 0 && state->min_width == state->max_width && state->min_height > 0 &&
            state->min_height == state->max_height);
}

/* Adds a window to the tiling of `output` (by default the one it is on), splitting `target`
 * when that is tiled there, else the tile under the pointer with `at_cursor`. */
static void tile_toplevel(struct sh_toplevel *toplevel, struct wlr_output *output,
                          struct sh_toplevel *target, bool at_cursor) {
    struct sh_server *server = toplevel->server;
    if (!output)
        output = toplevel_output(toplevel);
    if (!output || toplevel->tiled)
        return;
    if (!toplevel->arranged)
        toplevel->restore_box =
            toplevel->fullscreen ? toplevel->fullscreen_restore : toplevel_box(toplevel);
    toplevel->arranged = false;
    toplevel->tiled = true;
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel->foreign, false);
    sh_tiling_insert(server->tiling, output->name, toplevel->workspace, toplevel, target, at_cursor,
                     server->cursor->x, server->cursor->y);
    reflow_output(server, output);
}

/* Takes a window out of the tiling. `restore` returns it to its floating geometry; otherwise
 * it stays where it is, arranged without a rule, until something else places it. */
static void untile_toplevel(struct sh_toplevel *toplevel, bool restore) {
    struct sh_server *server = toplevel->server;
    if (!toplevel->tiled)
        return;
    struct wlr_output *output = tiled_output(toplevel);
    sh_tiling_remove(server->tiling, toplevel);
    toplevel->tiled = false;
    toplevel->arranged = !restore;
    toplevel->arrangement = SH_NONE;
    if (restore && toplevel->fullscreen) {
        toplevel->fullscreen_restore = toplevel->restore_box;
    } else if (restore) {
        toplevel_set_states(toplevel, false, 0);
        toplevel_configure_box(toplevel, toplevel->restore_box);
    }
    if (output && server->tiling_enabled)
        reflow_output(server, output);
}

static void set_tiling(struct sh_server *server, bool enabled) {
    if (server->tiling_enabled == enabled)
        return;
    server->tiling_enabled = enabled;
    // Most recently focused first, so the focused window gets the largest tile.
    struct sh_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (enabled && wants_tiling(toplevel))
            tile_toplevel(toplevel, NULL, NULL, false);
        else if (!enabled && toplevel->tiled)
            untile_toplevel(toplevel, true);
        else if (!enabled && toplevel->arranged && toplevel->arrangement == SH_NONE)
            restore_toplevel(toplevel); // left the tiling while minimized
    }
    wlr_log(WLR_INFO, "Tiling %s", enabled ? "on" : "off");
    notify_subscribers(server);
}

static void foreign_activate(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, foreign_activate);
    struct wlr_foreign_toplevel_handle_v1_activated_event *event = data;
    if (event->seat == toplevel->server->seat)
        focus_toplevel(toplevel);
}
static void foreign_close(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, foreign_close);
    toplevel_close(toplevel);
}
static void foreign_maximize(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, foreign_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
    if (!event->maximized) {
        restore_toplevel(toplevel);
        return;
    }
    if (toplevel->tiled) {
        toplevel->floating = true;
        untile_toplevel(toplevel, false);
    }
    place_maximized(toplevel);
}
static void foreign_fullscreen(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, foreign_fullscreen);
    struct wlr_foreign_toplevel_handle_v1_fullscreen_event *event = data;
    set_fullscreen(toplevel, event->fullscreen);
}
static void foreign_minimize(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, foreign_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
    if (event->minimized)
        minimize_toplevel(toplevel);
    else
        focus_toplevel(toplevel);
}
static void toplevel_request_minimize(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_minimize);
    if (toplevel_mapped(toplevel))
        minimize_toplevel(toplevel);
}
static void update_listed_state(struct sh_toplevel *toplevel) {
    if (!toplevel->listed)
        return;
    const char *title = toplevel_title(toplevel), *app_id = toplevel_app_id(toplevel);
    struct wlr_ext_foreign_toplevel_handle_v1_state state = {title ? title : "Untitled",
                                                             app_id ? app_id : ""};
    wlr_ext_foreign_toplevel_handle_v1_update_state(toplevel->listed, &state);
}
static void toplevel_title_changed(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, title_changed);
    const char *title = toplevel_title(toplevel);
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_title(toplevel->foreign, title ? title : "Untitled");
    update_listed_state(toplevel);
}
static void toplevel_app_id_changed(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, app_id_changed);
    const char *app_id = toplevel_app_id(toplevel);
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_app_id(toplevel->foreign, app_id ? app_id : "");
    update_listed_state(toplevel);
}
static void list_toplevel(struct sh_toplevel *toplevel) {
    struct sh_server *server = toplevel->server;
    toplevel->capture_scene = wlr_scene_create();
    if (!toplevel->capture_scene)
        return;
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface)
        wlr_scene_subsurface_tree_create(&toplevel->capture_scene->tree,
                                         toplevel_surface(toplevel));
    else
#endif
        wlr_scene_xdg_surface_create(&toplevel->capture_scene->tree, toplevel->xdg_toplevel->base);
    struct wlr_ext_foreign_toplevel_handle_v1_state state = {"", ""};
    toplevel->listed = wlr_ext_foreign_toplevel_handle_v1_create(server->toplevel_list, &state);
    if (toplevel->listed) {
        toplevel->listed->data = toplevel;
        update_listed_state(toplevel);
    }
}
static void unlist_toplevel(struct sh_toplevel *toplevel) {
    if (toplevel->listed)
        wlr_ext_foreign_toplevel_handle_v1_destroy(toplevel->listed);
    toplevel->listed = NULL;
    // Destroying the scene also ends any capture source made from it.
    if (toplevel->capture_scene)
        wlr_scene_node_destroy(&toplevel->capture_scene->tree.node);
    toplevel->capture_scene = NULL;
    toplevel->capture_source = NULL;
}
static void server_new_capture_request(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_capture_request);
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request = data;
    struct sh_toplevel *toplevel = request->toplevel_handle->data;
    if (!toplevel || !toplevel->capture_scene || server->locked)
        return;
    if (!toplevel->capture_source)
        toplevel->capture_source = wlr_ext_image_capture_source_v1_create_with_scene_node(
            &toplevel->capture_scene->tree.node, wl_display_get_event_loop(server->wl_display),
            server->allocator, server->renderer);
    if (toplevel->capture_source)
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(
            request, toplevel->capture_source);
}
static void publish_toplevel(struct sh_toplevel *toplevel) {
    list_toplevel(toplevel);
    toplevel->foreign = wlr_foreign_toplevel_handle_v1_create(toplevel->server->foreign_manager);
    if (!toplevel->foreign)
        return;
    toplevel_title_changed(&toplevel->title_changed, NULL);
    toplevel_app_id_changed(&toplevel->app_id_changed, NULL);
    add_listener(&toplevel->foreign->events.request_activate, &toplevel->foreign_activate,
                 foreign_activate);
    add_listener(&toplevel->foreign->events.request_close, &toplevel->foreign_close, foreign_close);
    add_listener(&toplevel->foreign->events.request_maximize, &toplevel->foreign_maximize,
                 foreign_maximize);
    add_listener(&toplevel->foreign->events.request_minimize, &toplevel->foreign_minimize,
                 foreign_minimize);
    add_listener(&toplevel->foreign->events.request_fullscreen, &toplevel->foreign_fullscreen,
                 foreign_fullscreen);
    wlr_foreign_toplevel_handle_v1_set_fullscreen(toplevel->foreign, toplevel->fullscreen);
    struct wlr_output *output = toplevel_output(toplevel);
    if (output)
        wlr_foreign_toplevel_handle_v1_output_enter(toplevel->foreign, output);
}
static void unpublish_toplevel(struct sh_toplevel *toplevel) {
    unlist_toplevel(toplevel);
    if (!toplevel->foreign)
        return;
    wl_list_remove(&toplevel->foreign_activate.link);
    wl_list_remove(&toplevel->foreign_close.link);
    wl_list_remove(&toplevel->foreign_maximize.link);
    wl_list_remove(&toplevel->foreign_minimize.link);
    wl_list_remove(&toplevel->foreign_fullscreen.link);
    wlr_foreign_toplevel_handle_v1_destroy(toplevel->foreign);
    toplevel->foreign = NULL;
}

/* Reserve exclusive panel regions before positioning nonexclusive layers. */
static void arrange_layers(struct sh_server *server) {
    struct sh_output *output;
    struct sh_layer *exclusive = NULL;
    wl_list_for_each(output, &server->outputs, link) {
        struct wlr_box full, usable;
        wlr_output_layout_get_box(server->output_layout, output->wlr_output, &full);
        usable = full;
        for (int pass = 0; pass < 2; ++pass) {
            for (int level = 3; level >= 0; --level) {
                struct sh_layer *layer;
                wl_list_for_each(layer, &server->layers, link) {
                    struct wlr_layer_surface_v1 *surface = layer->surface;
                    if (surface->output != output->wlr_output || !surface->initialized ||
                        (!surface->surface->mapped && !surface->initial_commit) ||
                        surface->current.layer != (unsigned)level ||
                        (surface->current.exclusive_zone > 0) != (pass == 0))
                        continue;
                    wlr_scene_node_reparent(&layer->scene->tree->node, server->layer_trees[level]);
                    wlr_scene_layer_surface_v1_configure(layer->scene, &full, &usable);
                    if (!exclusive && surface->surface->mapped && level >= 2 &&
                        surface->current.keyboard_interactive ==
                            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
                        exclusive = layer;
                }
            }
        }
        if (!wlr_box_equal(&output->usable, &usable)) {
            output->usable = usable;
            reflow_output(server, output->wlr_output);
        }
    }
    if (exclusive)
        focus_layer(exclusive);
    else if (server->focused_layer &&
             (!server->focused_layer->surface->surface->mapped ||
              server->focused_layer->surface->current.keyboard_interactive ==
                  ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE))
        focus_previous(server);
}
static void layer_commit(struct wl_listener *listener, void *data) {
    struct sh_layer *layer = wl_container_of(listener, layer, commit);
    if (layer->surface->initial_commit || layer->surface->current.committed)
        arrange_layers(layer->server);
}
static void layer_map(struct wl_listener *listener, void *data) {
    struct sh_layer *layer = wl_container_of(listener, layer, map);
    arrange_layers(layer->server);
}
static void layer_unmap(struct wl_listener *listener, void *data) {
    struct sh_layer *layer = wl_container_of(listener, layer, unmap);
    if (layer->server->focused_layer == layer)
        focus_previous(layer->server);
    arrange_layers(layer->server);
}
static void layer_destroy(struct wl_listener *listener, void *data) {
    struct sh_layer *layer = wl_container_of(listener, layer, destroy);
    struct sh_server *server = layer->server;
    if (server->focused_layer == layer)
        focus_previous(server);
    wl_list_remove(&layer->commit.link);
    wl_list_remove(&layer->map.link);
    wl_list_remove(&layer->unmap.link);
    wl_list_remove(&layer->destroy.link);
    wl_list_remove(&layer->new_popup.link);
    wl_list_remove(&layer->link);
    free(layer);
    arrange_layers(server);
}
static void layer_new_popup(struct wl_listener *listener, void *data) {
    struct sh_layer *layer = wl_container_of(listener, layer, new_popup);
    create_popup(layer->server, data, layer->scene->tree);
}
static void server_new_layer_surface(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *surface = data;
    if (!surface->output && !(surface->output = first_output(server))) {
        wlr_layer_surface_v1_destroy(surface);
        return;
    }
    struct sh_layer *layer = calloc(1, sizeof(*layer));
    if (!layer) {
        wlr_layer_surface_v1_destroy(surface);
        return;
    }
    layer->server = server;
    layer->surface = surface;
    layer->node = (struct sh_node){SH_NODE_LAYER, layer};
    layer->scene =
        wlr_scene_layer_surface_v1_create(server->layer_trees[surface->pending.layer], surface);
    if (!layer->scene) {
        free(layer);
        wlr_layer_surface_v1_destroy(surface);
        return;
    }
    layer->scene->tree->node.data = &layer->node;
    surface->data = layer;
    wl_list_insert(&server->layers, &layer->link);
    add_listener(&surface->surface->events.commit, &layer->commit, layer_commit);
    add_listener(&surface->surface->events.map, &layer->map, layer_map);
    add_listener(&surface->surface->events.unmap, &layer->unmap, layer_unmap);
    add_listener(&surface->events.destroy, &layer->destroy, layer_destroy);
    add_listener(&surface->events.new_popup, &layer->new_popup, layer_new_popup);
}

static void maximize_toplevel(struct sh_toplevel *toplevel, bool maximized) {
    if (toplevel->tiled) {
        toplevel_refresh(toplevel); // Tiles ignore client maximize requests, as in Hyprland.
        return;
    }
    if (maximized)
        place_maximized(toplevel);
    else
        restore_toplevel(toplevel);
}

static void map_toplevel(struct sh_toplevel *toplevel, bool fullscreen, bool maximized) {
    struct sh_server *server = toplevel->server;
    struct sh_toplevel *previous = server->focused_toplevel;
    toplevel->workspace = toplevel->server->workspace;
    int offset = 40 + 32 * (wl_list_length(&toplevel->server->toplevels) % 8);
    int x = offset, y = offset;
    bool resize = false;
    struct wlr_box geometry = toplevel_geometry(toplevel);
    int width = geometry.width, height = geometry.height;
    // New windows open on the output under the pointer, as in Hyprland.
    struct wlr_output *output =
        wlr_output_layout_output_at(server->output_layout, server->cursor->x, server->cursor->y);
    if (!output)
        output = toplevel_output(toplevel);
    if (output) {
        struct sh_rect area = usable_area(server, output);
        // Keep newly opened applications reachable inside a small nested output.
        int margin = area.width < 80 || area.height < 80 ? 0 : 40;
        if (width > area.width - 2 * margin) {
            width = area.width - 2 * margin;
            resize = true;
        }
        if (height > area.height - 2 * margin) {
            height = area.height - 2 * margin;
            resize = true;
        }
        x = area.x + (offset + width <= area.width ? offset : margin);
        y = area.y + (offset + height <= area.height ? offset : margin);
    }
    if (resize && width > 0 && height > 0)
        toplevel_configure(toplevel, x, y, width, height);
    else
        toplevel_set_position(toplevel, x, y);
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);

    publish_toplevel(toplevel);
    toplevel->floating = toplevel_is_dialog(toplevel);
    if (wants_tiling(toplevel)) {
        // As in Hyprland: split the focused tile when it is on the pointer's output, else the
        // tile under the pointer.
        bool split_focused = previous && previous->tiled && toplevel_visible(previous) &&
                             (!output || tiled_output(previous) == output);
        tile_toplevel(toplevel, split_focused ? tiled_output(previous) : output,
                      split_focused ? previous : NULL, true);
    }
    focus_toplevel(toplevel);
    if (fullscreen)
        set_fullscreen(toplevel, true);
    else if (maximized)
        maximize_toplevel(toplevel, true);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    map_toplevel(toplevel, toplevel->xdg_toplevel->requested.fullscreen,
                 toplevel->xdg_toplevel->requested.maximized);
}

static void unmap_toplevel(struct sh_toplevel *toplevel) {
    if (toplevel == toplevel->server->grabbed_toplevel) {
        reset_cursor_mode(toplevel->server);
    }
    forget_decoration(toplevel);

    toplevel->fullscreen = false;
    untile_toplevel(toplevel, false);
    bool was_focused = toplevel->server->focused_toplevel == toplevel;
    if (was_focused)
        deactivate_toplevel(toplevel->server);
    unpublish_toplevel(toplevel);
    wl_list_remove(&toplevel->link);
    if (was_focused)
        focus_previous(toplevel->server);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    unmap_toplevel(toplevel);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

    if (toplevel->xdg_toplevel->base->initial_commit) {
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    }
}

/* Frees a window after removing the listeners xdg-shell and X11 windows have in common. */
static void free_toplevel(struct sh_toplevel *toplevel) {
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->request_minimize.link);
    wl_list_remove(&toplevel->title_changed.link);
    wl_list_remove(&toplevel->app_id_changed.link);
    free(toplevel);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    free_toplevel(toplevel);
}

static void begin_interactive(struct sh_toplevel *toplevel, enum sh_cursor_mode mode,
                              uint32_t edges) {
    struct sh_server *server = toplevel->server;
    if (toplevel->fullscreen)
        return;

    // Resizing a tile moves its splits; moving one lifts it out until it is dropped.
    bool tiled_resize = toplevel->tiled && mode == SH_CURSOR_RESIZE;
    bool retile = toplevel->tiled && mode == SH_CURSOR_MOVE;
    if (retile)
        untile_toplevel(toplevel, false);
    bool was_arranged = toplevel->arranged;
    if (!tiled_resize) {
        toplevel->arranged = false;
        toplevel_set_states(toplevel, false, 0);
    }
    server->grab_retile = retile;
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == SH_CURSOR_MOVE && was_arranged) {
        /* Dragging a maximized or snapped window restores its floating size, keeping the
         * pointer at the same relative spot across the width and at most as far down. */
        struct wlr_box geometry = toplevel_geometry(toplevel);
        struct wlr_box restore = toplevel->restore_box;
        double from_left = server->cursor->x - toplevel->scene_tree->node.x;
        double from_top = server->cursor->y - toplevel->scene_tree->node.y;
        if (geometry.width > 0)
            from_left = from_left * restore.width / geometry.width;
        if (from_top > restore.height)
            from_top = restore.height / 2.0;
        toplevel_configure(toplevel, server->cursor->x - from_left, server->cursor->y - from_top,
                           restore.width, restore.height);
    }

    if (mode == SH_CURSOR_MOVE) {
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        struct wlr_box geo_box = toplevel_geometry(toplevel);

        edges = corner_edges(toplevel, edges);

        double border_x = (toplevel->scene_tree->node.x + geo_box.x) +
                          ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        double border_y = (toplevel->scene_tree->node.y + geo_box.y) +
                          ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = geo_box;
        server->grab_geobox.x += toplevel->scene_tree->node.x;
        server->grab_geobox.y += toplevel->scene_tree->node.y;

        server->resize_edges = edges;
    }
}

/* Client decorations often live in subsurfaces (kitty's title bar), so the clicked surface
 * only has to belong to the toplevel, not be its root surface. */
static bool validate_grab_serial(struct sh_toplevel *toplevel, uint32_t serial) {
    struct wlr_seat *seat = toplevel->server->seat;
    struct wlr_surface *focused = seat->pointer_state.focused_surface;
    return wlr_seat_validate_pointer_grab_serial(seat, NULL, serial) && focused &&
           wlr_surface_get_root_surface(focused) == toplevel->xdg_toplevel->base->surface;
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    struct wlr_xdg_toplevel_move_event *event = data;
    if (validate_grab_serial(toplevel, event->serial))
        begin_interactive(toplevel, SH_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    if (validate_grab_serial(toplevel, event->serial))
        begin_interactive(toplevel, SH_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    if (!toplevel->xdg_toplevel->base->initialized)
        return;
    if (!toplevel->fullscreen)
        maximize_toplevel(toplevel, toplevel->xdg_toplevel->requested.maximized);
    wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

/* Fullscreen covers the whole output, including exclusive panel zones. */
static void fit_fullscreen(struct sh_toplevel *toplevel) {
    struct wlr_output *output = toplevel_output(toplevel);
    if (!output)
        return;
    struct wlr_box box;
    wlr_output_layout_get_box(toplevel->server->output_layout, output, &box);
    toplevel_configure_box(toplevel, box);
}

static void refit_fullscreen(struct sh_server *server) {
    struct sh_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel->fullscreen)
            fit_fullscreen(toplevel);
    }
}

static void set_fullscreen(struct sh_toplevel *toplevel, bool fullscreen) {
    struct sh_server *server = toplevel->server;
    if (!toplevel_mapped(toplevel) || toplevel->fullscreen == fullscreen) {
        toplevel_refresh(toplevel);
        return;
    }
    if (server->grabbed_toplevel == toplevel)
        reset_cursor_mode(server);
    if (fullscreen)
        toplevel->fullscreen_restore = toplevel_box(toplevel);
    toplevel->fullscreen = fullscreen;
    toplevel_set_fullscreen_state(toplevel, fullscreen);
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_fullscreen(toplevel->foreign, fullscreen);
    if (fullscreen) {
        fit_fullscreen(toplevel);
    } else {
        toplevel_configure_box(toplevel, toplevel->fullscreen_restore);
        wlr_scene_node_reparent(&toplevel->scene_tree->node, server->windows);
        // The usable area may have changed while this window covered the output.
        struct wlr_output *output =
            toplevel->tiled ? tiled_output(toplevel) : toplevel_output(toplevel);
        if ((toplevel->arranged || toplevel->tiled) && output)
            reflow_output(server, output);
    }
    if (server->focused_toplevel == toplevel || fullscreen)
        focus_toplevel(toplevel);
    refresh_decoration(toplevel);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    set_fullscreen(toplevel, toplevel->xdg_toplevel->requested.fullscreen);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct sh_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->scene_tree =
        wlr_scene_xdg_surface_create(toplevel->server->windows, xdg_toplevel->base);
    toplevel->node = (struct sh_node){SH_NODE_TOPLEVEL, toplevel};
    toplevel->scene_tree->node.data = &toplevel->node;
    xdg_toplevel->base->data = toplevel->scene_tree;

    struct wlr_surface *surface = xdg_toplevel->base->surface;
    add_listener(&surface->events.map, &toplevel->map, xdg_toplevel_map);
    add_listener(&surface->events.unmap, &toplevel->unmap, xdg_toplevel_unmap);
    add_listener(&surface->events.commit, &toplevel->commit, xdg_toplevel_commit);
    add_listener(&xdg_toplevel->events.destroy, &toplevel->destroy, xdg_toplevel_destroy);
    add_listener(&xdg_toplevel->events.set_title, &toplevel->title_changed, toplevel_title_changed);
    add_listener(&xdg_toplevel->events.set_app_id, &toplevel->app_id_changed,
                 toplevel_app_id_changed);
    add_listener(&xdg_toplevel->events.request_move, &toplevel->request_move,
                 xdg_toplevel_request_move);
    add_listener(&xdg_toplevel->events.request_resize, &toplevel->request_resize,
                 xdg_toplevel_request_resize);
    add_listener(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize,
                 xdg_toplevel_request_maximize);
    add_listener(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen,
                 xdg_toplevel_request_fullscreen);
    add_listener(&xdg_toplevel->events.request_minimize, &toplevel->request_minimize,
                 toplevel_request_minimize);
}

#if WLR_HAS_XWAYLAND
/* X11 windows: managed ones behave like xdg toplevels; override-redirect ones
 * (menus, tooltips, drag icons) are drawn where they ask and never take part in focus order. */
static void xwayland_map(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    struct wlr_xwayland_surface *xsurface = toplevel->xsurface;
    struct sh_server *server = toplevel->server;
    toplevel->unmanaged = xsurface->override_redirect;
    toplevel->scene_tree =
        wlr_scene_tree_create(toplevel->unmanaged ? server->unmanaged : server->windows);
    if (!toplevel->scene_tree ||
        !wlr_scene_subsurface_tree_create(toplevel->scene_tree, xsurface->surface)) {
        wlr_log(WLR_ERROR, "Cannot create scene for X11 window");
        if (toplevel->scene_tree)
            wlr_scene_node_destroy(&toplevel->scene_tree->node);
        toplevel->scene_tree = NULL;
        return;
    }
    if (toplevel->unmanaged) {
        wlr_scene_node_set_position(&toplevel->scene_tree->node, xsurface->x, xsurface->y);
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
        if (keyboard && !server->locked &&
            wlr_xwayland_surface_override_redirect_wants_focus(xsurface))
            wlr_seat_keyboard_notify_enter(server->seat, xsurface->surface, keyboard->keycodes,
                                           keyboard->num_keycodes, &keyboard->modifiers);
        return;
    }
    toplevel->scene_tree->node.data = &toplevel->node;
    map_toplevel(toplevel, xsurface->fullscreen,
                 xsurface->maximized_horz && xsurface->maximized_vert);
    refresh_decoration(toplevel);
}

static void xwayland_unmap(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    struct sh_server *server = toplevel->server;
    if (!toplevel->scene_tree)
        return;
    if (toplevel->unmanaged) {
        // Return the keyboard from a closed X11 menu to the focused window.
        if (server->seat->keyboard_state.focused_surface == toplevel->xsurface->surface) {
            if (server->focused_toplevel)
                focus_toplevel(server->focused_toplevel);
            else
                wlr_seat_keyboard_clear_focus(server->seat);
        }
    } else {
        unmap_toplevel(toplevel);
    }
    wlr_scene_node_destroy(&toplevel->scene_tree->node);
    toplevel->scene_tree = NULL;
}

static void xwayland_associate(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, x_associate);
    toplevel->associated = true;
    add_listener(&toplevel->xsurface->surface->events.map, &toplevel->map, xwayland_map);
    add_listener(&toplevel->xsurface->surface->events.unmap, &toplevel->unmap, xwayland_unmap);
}

static void xwayland_dissociate(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, x_dissociate);
    toplevel->associated = false;
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
}

static void xwayland_destroy(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    if (toplevel->associated) {
        wl_list_remove(&toplevel->map.link);
        wl_list_remove(&toplevel->unmap.link);
    }
    wl_list_remove(&toplevel->x_associate.link);
    wl_list_remove(&toplevel->x_dissociate.link);
    wl_list_remove(&toplevel->x_configure.link);
    wl_list_remove(&toplevel->x_activate.link);
    wl_list_remove(&toplevel->x_geometry.link);
    wl_list_remove(&toplevel->x_decorations.link);
    free_toplevel(toplevel);
}

static bool xwayland_managed(struct sh_toplevel *toplevel) {
    return toplevel_mapped(toplevel) && !toplevel->unmanaged;
}

static void xwayland_set_decorations(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, x_decorations);
    if (xwayland_managed(toplevel))
        refresh_decoration(toplevel);
}

static void xwayland_request_configure(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, x_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    if (!xwayland_managed(toplevel)) {
        wlr_xwayland_surface_configure(toplevel->xsurface, event->x, event->y, event->width,
                                       event->height);
        if (toplevel->scene_tree)
            wlr_scene_node_set_position(&toplevel->scene_tree->node, event->x, event->y);
        return;
    }
    // Placement belongs to the compositor; floating windows may still choose their size.
    if (toplevel->fullscreen || toplevel->arranged || toplevel->tiled) {
        toplevel_refresh(toplevel);
        return;
    }
    toplevel_configure(toplevel, toplevel->scene_tree->node.x, toplevel->scene_tree->node.y,
                       event->width, event->height);
}

static void xwayland_set_geometry(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, x_geometry);
    if (toplevel->unmanaged && toplevel->scene_tree)
        wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->xsurface->x,
                                    toplevel->xsurface->y);
}

static void xwayland_request_activate(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, x_activate);
    if (xwayland_managed(toplevel))
        focus_toplevel(toplevel);
}

/* X11 grab requests carry no serial; accept them only while a button is held. */
static void xwayland_request_move(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    if (xwayland_managed(toplevel) && toplevel->server->seat->pointer_state.button_count > 0)
        begin_interactive(toplevel, SH_CURSOR_MOVE, 0);
}

static void xwayland_request_resize(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    struct wlr_xwayland_resize_event *event = data;
    if (xwayland_managed(toplevel) && toplevel->server->seat->pointer_state.button_count > 0)
        begin_interactive(toplevel, SH_CURSOR_RESIZE, event->edges);
}

static void xwayland_request_maximize(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    if (!xwayland_managed(toplevel) || toplevel->fullscreen)
        return;
    maximize_toplevel(toplevel,
                      toplevel->xsurface->maximized_horz || toplevel->xsurface->maximized_vert);
}

static void xwayland_request_fullscreen(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    if (xwayland_managed(toplevel))
        set_fullscreen(toplevel, toplevel->xsurface->fullscreen);
}

static void xwayland_request_minimize(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_minimize);
    struct wlr_xwayland_minimize_event *event = data;
    if (!xwayland_managed(toplevel))
        return;
    if (event->minimize)
        minimize_toplevel(toplevel);
    else
        focus_toplevel(toplevel);
}

static void server_new_xwayland_surface(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface *xsurface = data;
    struct sh_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    if (!toplevel) {
        wlr_xwayland_surface_close(xsurface);
        return;
    }
    toplevel->server = server;
    toplevel->xsurface = xsurface;
    toplevel->node = (struct sh_node){SH_NODE_TOPLEVEL, toplevel};
    xsurface->data = toplevel;
    add_listener(&xsurface->events.associate, &toplevel->x_associate, xwayland_associate);
    add_listener(&xsurface->events.dissociate, &toplevel->x_dissociate, xwayland_dissociate);
    add_listener(&xsurface->events.destroy, &toplevel->destroy, xwayland_destroy);
    add_listener(&xsurface->events.request_configure, &toplevel->x_configure,
                 xwayland_request_configure);
    add_listener(&xsurface->events.request_activate, &toplevel->x_activate,
                 xwayland_request_activate);
    add_listener(&xsurface->events.set_geometry, &toplevel->x_geometry, xwayland_set_geometry);
    add_listener(&xsurface->events.set_decorations, &toplevel->x_decorations,
                 xwayland_set_decorations);
    add_listener(&xsurface->events.set_title, &toplevel->title_changed, toplevel_title_changed);
    add_listener(&xsurface->events.set_class, &toplevel->app_id_changed, toplevel_app_id_changed);
    add_listener(&xsurface->events.request_move, &toplevel->request_move, xwayland_request_move);
    add_listener(&xsurface->events.request_resize, &toplevel->request_resize,
                 xwayland_request_resize);
    add_listener(&xsurface->events.request_maximize, &toplevel->request_maximize,
                 xwayland_request_maximize);
    add_listener(&xsurface->events.request_fullscreen, &toplevel->request_fullscreen,
                 xwayland_request_fullscreen);
    add_listener(&xsurface->events.request_minimize, &toplevel->request_minimize,
                 xwayland_request_minimize);
}

#if SHAODE_XWM_WAKER
/* wlroots' XWM can strand X events: xcb reads them into its queue during flushes
 * and round-trips outside the event handler, and the handler's post-dispatch
 * check ignores that queue (packaging/patches/wlroots-xwm-drain.patch fixes it).
 * That strands the first MapRequest after Xwayland starts, among others. While
 * Xwayland runs, a periodic client message from a separate connection, sent only
 * to the XWM's own window, makes its socket readable so the handler drains the queue. */
enum { XWM_WAKE_INTERVAL_MS = 250 };

static void close_xwm_waker(struct sh_server *server) {
    if (server->waker_timer)
        wl_event_source_remove(server->waker_timer);
    if (server->waker_input)
        wl_event_source_remove(server->waker_input);
    if (server->xwm_waker)
        xcb_disconnect(server->xwm_waker);
    server->waker_timer = server->waker_input = NULL;
    server->xwm_waker = NULL;
}

static int xwm_waker_tick(void *data) {
    struct sh_server *server = data;
    xcb_client_message_event_t message = {.response_type = XCB_CLIENT_MESSAGE,
                                          .format = 32,
                                          .window = server->xwm_window,
                                          .type = server->waker_atom};
    // An empty event mask delivers the message only to the window's creator: the XWM.
    xcb_send_event(server->xwm_waker, false, server->xwm_window, XCB_EVENT_MASK_NO_EVENT,
                   (const char *)&message);
    xcb_flush(server->xwm_waker);
    wl_event_source_timer_update(server->waker_timer, XWM_WAKE_INTERVAL_MS);
    return 0;
}

static int xwm_waker_input(int fd, uint32_t mask, void *data) {
    struct sh_server *server = data;
    xcb_generic_event_t *event;
    while ((event = xcb_poll_for_event(server->xwm_waker)))
        free(event); // Only errors can arrive; no events are selected.
    if ((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) || xcb_connection_has_error(server->xwm_waker))
        close_xwm_waker(server);
    return 0;
}

static void open_xwm_waker(struct sh_server *server) {
    close_xwm_waker(server);
    server->xwm_waker = xcb_connect(server->xwayland->display_name, NULL);
    if (xcb_connection_has_error(server->xwm_waker)) {
        xcb_disconnect(server->xwm_waker);
        server->xwm_waker = NULL;
        wlr_log(WLR_ERROR, "Cannot connect XWM waker; X11 windows may appear late");
        return;
    }
    xcb_atom_t atoms[2] = {XCB_ATOM_NONE, XCB_ATOM_NONE};
    const char *names[2] = {"_SHAODE_XWM_WAKE", "_NET_SUPPORTING_WM_CHECK"};
    for (int i = 0; i < 2; ++i) {
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
            server->xwm_waker,
            xcb_intern_atom(server->xwm_waker, false, strlen(names[i]), names[i]), NULL);
        if (reply)
            atoms[i] = reply->atom;
        free(reply);
    }
    server->waker_atom = atoms[0];
    // Like the XWM's connection, this one must not keep an idle Xwayland running.
    xcb_xfixes_query_version_reply_t *xfixes = xcb_xfixes_query_version_reply(
        server->xwm_waker, xcb_xfixes_query_version(server->xwm_waker, 6, 0), NULL);
    if (xfixes && xfixes->major_version >= 6)
        xcb_xfixes_set_client_disconnect_mode(server->xwm_waker,
                                              XCB_XFIXES_CLIENT_DISCONNECT_FLAGS_TERMINATE);
    free(xfixes);
    server->xwm_window = XCB_WINDOW_NONE;
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(server->xwm_waker)).data;
    xcb_get_property_reply_t *check = xcb_get_property_reply(
        server->xwm_waker,
        xcb_get_property(server->xwm_waker, false, screen->root, atoms[1], XCB_ATOM_WINDOW, 0, 1),
        NULL);
    if (check && xcb_get_property_value_length(check) == sizeof(xcb_window_t))
        server->xwm_window = *(xcb_window_t *)xcb_get_property_value(check);
    free(check);
    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    server->waker_input = wl_event_loop_add_fd(loop, xcb_get_file_descriptor(server->xwm_waker),
                                               WL_EVENT_READABLE, xwm_waker_input, server);
    server->waker_timer = wl_event_loop_add_timer(loop, xwm_waker_tick, server);
    if (server->waker_atom == XCB_ATOM_NONE || server->xwm_window == XCB_WINDOW_NONE ||
        !server->waker_input || !server->waker_timer) {
        wlr_log(WLR_ERROR, "Cannot set up XWM waker; X11 windows may appear late");
        close_xwm_waker(server);
        return;
    }
    xwm_waker_tick(server); // drain whatever the XWM stranded while attaching
}
#endif

static void xwayland_ready(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, xwayland_ready);
    wlr_log(WLR_INFO, "XWayland ready on DISPLAY=%s", server->xwayland->display_name);
    wlr_xwayland_set_seat(server->xwayland, server->seat);
#if SHAODE_XWM_WAKER
    open_xwm_waker(server);
#endif
    if (wlr_xcursor_manager_load(server->cursor_mgr, 1)) {
        struct wlr_xcursor *xcursor =
            wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "default", 1);
        if (xcursor) {
            struct wlr_xcursor_image *image = xcursor->images[0];
            wlr_xwayland_set_cursor(server->xwayland, wlr_xcursor_image_get_buffer(image),
                                    image->hotspot_x, image->hotspot_y);
        }
    }
}
#endif

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    struct sh_popup *popup = wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit) {
        // Keep menus on the output of their window or panel; positioners say how to flip or slide.
        struct wlr_scene_tree *root = popup->xdg_popup->base->data;
        while (root && !root->node.data)
            root = root->node.parent;
        struct sh_server *server = popup->server;
        int root_x = 0, root_y = 0;
        if (root)
            wlr_scene_node_coords(&root->node, &root_x, &root_y);
        else
            root_x = server->cursor->x, root_y = server->cursor->y;
        struct wlr_output *output =
            wlr_output_layout_output_at(server->output_layout, root_x, root_y);
        if (!output)
            output = wlr_output_layout_output_at(server->output_layout, server->cursor->x,
                                                 server->cursor->y);
        if (root && output) {
            struct wlr_box box;
            wlr_output_layout_get_box(server->output_layout, output, &box);
            box.x -= root_x;
            box.y -= root_y;
            wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &box);
        }
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    struct sh_popup *popup = wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);

    free(popup);
}

static void create_popup(struct sh_server *server, struct wlr_xdg_popup *xdg_popup,
                         struct wlr_scene_tree *parent_tree) {
    struct sh_popup *popup = calloc(1, sizeof(*popup));
    popup->server = server;
    popup->xdg_popup = xdg_popup;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    add_listener(&xdg_popup->base->surface->events.commit, &popup->commit, xdg_popup_commit);
    add_listener(&xdg_popup->events.destroy, &popup->destroy, xdg_popup_destroy);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_xdg_popup);
    struct wlr_xdg_popup *popup = data;
    // A layer-shell popup is attached by the layer's new_popup handler instead.
    if (!popup->parent)
        return;
    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (parent && parent->data)
        create_popup(server, popup, parent->data);
}

/* Control socket: one newline-terminated request per connection, answered with
 * "ok\n" plus any output, or "error: ...\n". Lives in the private runtime dir. */
struct sh_control_client {
    struct sh_server *server;
    int fd;
    struct wl_event_source *source;
    bool subscribed; // "subscribe": stays open and receives the state after each change
    struct wl_list link;
    size_t length;
    char request[512];
};

static void control_reply(int fd, const char *text) {
    size_t length = strlen(text);
    while (length > 0) {
        ssize_t written = send(fd, text, length, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return;
        text += written;
        length -= (size_t)written;
    }
}

static void control_describe_windows(struct sh_server *server, int fd) {
    control_reply(fd, "ok\n");
    struct sh_toplevel *toplevel;
    // workspace, focused, minimized, tiled, x, y, width, height, app_id, title — one per line.
    wl_list_for_each_reverse(toplevel, &server->toplevels, link) {
        char line[1024];
        const char *app_id = toplevel_app_id(toplevel), *title = toplevel_title(toplevel);
        struct wlr_box geometry = toplevel_geometry(toplevel);
        snprintf(line, sizeof(line), "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n",
                 toplevel->workspace + 1, server->focused_toplevel == toplevel, toplevel->minimized,
                 toplevel->tiled, toplevel->scene_tree->node.x + geometry.x,
                 toplevel->scene_tree->node.y + geometry.y, geometry.width, geometry.height,
                 app_id ? app_id : "", title ? title : "");
        for (char *c = line; c[0] && c[1]; ++c)
            if (*c == '\n' || *c == '\r')
                *c = ' ';
        control_reply(fd, line);
    }
}

static void control_handle(struct sh_server *server, int fd, const char *request) {
    if (!strcmp(request, "get workspace")) {
        char reply[32];
        snprintf(reply, sizeof(reply), "ok\n%d\n", server->workspace + 1);
        control_reply(fd, reply);
        return;
    }
    if (!strcmp(request, "get tiling")) {
        control_reply(fd, server->tiling_enabled ? "ok\non\n" : "ok\noff\n");
        return;
    }
    if (!strcmp(request, "get windows")) {
        control_describe_windows(server, fd);
        return;
    }
    if (server->locked) {
        control_reply(fd, "error: the session is locked\n");
        return;
    }
    char error[256] = "";
    int argument = 0;
    enum sh_action action = server->callbacks->command(server->callbacks->userdata, request,
                                                       &argument, error, sizeof(error));
    if (action == SH_NONE) {
        char reply[300];
        snprintf(reply, sizeof(reply), "error: %s\n", error[0] ? error : "unknown request");
        control_reply(fd, reply);
        return;
    }
    run_action(server, action, argument);
    control_reply(fd, "ok\n");
}

static void control_client_close(struct sh_control_client *client) {
    if (client->subscribed)
        wl_list_remove(&client->link);
    wl_event_source_remove(client->source);
    close(client->fd);
    free(client);
}

/* Subscribers get "tiling on|off" and "workspace N" lines; a subscriber that cannot keep up
 * is dropped rather than blocking the compositor. */
static bool control_send_state(struct sh_control_client *client) {
    struct sh_server *server = client->server;
    char state[64];
    int length = snprintf(state, sizeof(state), "tiling %s\nworkspace %d\n",
                          server->tiling_enabled ? "on" : "off", server->workspace + 1);
    return send(client->fd, state, (size_t)length, MSG_NOSIGNAL | MSG_DONTWAIT) == length;
}

static void notify_subscribers(struct sh_server *server) {
    struct sh_control_client *client, *temporary;
    wl_list_for_each_safe(client, temporary, &server->subscribers, link) {
        if (!control_send_state(client))
            control_client_close(client);
    }
}

static int control_client_readable(int fd, uint32_t mask, void *data) {
    struct sh_control_client *client = data;
    if (client->subscribed) {
        char ignored[64];
        ssize_t count = read(fd, ignored, sizeof(ignored));
        if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR))
            control_client_close(client);
        return 0;
    }
    ssize_t count =
        read(fd, client->request + client->length, sizeof(client->request) - 1 - client->length);
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    if (count <= 0) {
        control_client_close(client);
        return 0;
    }
    client->length += (size_t)count;
    client->request[client->length] = '\0';
    char *newline = strchr(client->request, '\n');
    if (!newline && client->length < sizeof(client->request) - 1)
        return 0;
    if (newline)
        *newline = '\0';
    if (newline && !strcmp(client->request, "subscribe")) {
        client->subscribed = true;
        wl_list_insert(&client->server->subscribers, &client->link);
        if (send(fd, "ok\n", 3, MSG_NOSIGNAL | MSG_DONTWAIT) != 3 || !control_send_state(client))
            control_client_close(client);
        return 0;
    }
    // Replies are small; a blocking write keeps the protocol simple.
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    if (newline)
        control_handle(client->server, fd, client->request);
    else
        control_reply(fd, "error: request too long\n");
    control_client_close(client);
    return 0;
}

static int control_accept(int fd, uint32_t mask, void *data) {
    struct sh_server *server = data;
    int client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client_fd < 0)
        return 0;
    struct sh_control_client *client = calloc(1, sizeof(*client));
    if (!client) {
        close(client_fd);
        return 0;
    }
    client->server = server;
    client->fd = client_fd;
    client->source = wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display), client_fd,
                                          WL_EVENT_READABLE, control_client_readable, client);
    if (!client->source) {
        close(client_fd);
        free(client);
    }
    return 0;
}

static void open_control_socket(struct sh_server *server, const char *wayland_socket) {
    server->control_fd = -1;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (!runtime || !*runtime ||
        snprintf(server->control_path, sizeof(server->control_path), "%s/shaode.%s.sock", runtime,
                 wayland_socket) >= (int)sizeof(server->control_path) ||
        strlen(server->control_path) >= sizeof(address.sun_path)) {
        wlr_log(WLR_ERROR, "No usable XDG_RUNTIME_DIR; control socket disabled");
        server->control_path[0] = '\0';
        return;
    }
    strcpy(address.sun_path, server->control_path);
    unlink(server->control_path); // A stale socket from a crashed session.
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 8) < 0) {
        wlr_log_errno(WLR_ERROR, "Cannot create control socket %s", server->control_path);
        if (fd >= 0)
            close(fd);
        server->control_path[0] = '\0';
        return;
    }
    server->control_fd = fd;
    server->control_source = wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display), fd,
                                                  WL_EVENT_READABLE, control_accept, server);
    setenv("SHAODE_SOCKET", server->control_path, true);
    wlr_log(WLR_INFO, "Control socket: %s", server->control_path);
}

static void close_control_socket(struct sh_server *server) {
    struct sh_control_client *client, *temporary;
    wl_list_for_each_safe(client, temporary, &server->subscribers, link) {
        control_client_close(client);
    }
    if (server->control_source)
        wl_event_source_remove(server->control_source);
    if (server->control_fd >= 0)
        close(server->control_fd);
    if (server->control_path[0])
        unlink(server->control_path);
}

static int terminate_signal(int signal_number, void *data) {
    struct sh_server *server = data;
    wl_display_terminate(server->wl_display);
    return 0;
}
static int reload_signal(int signal_number, void *data) {
    reload_config(data);
    return 0;
}
static int reap_children(int signal_number, void *data) {
    struct sh_server *server = data;
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        if (server->callbacks->child_exited)
            server->callbacks->child_exited(server->callbacks->userdata, pid);
    }
    return 0;
}

int sh_run(const struct sh_callbacks *callbacks, enum sh_backend_mode mode) {
    wlr_log_init(WLR_INFO, NULL);
    if (mode == SH_BACKEND_SESSION &&
        !(WLR_HAS_SESSION && WLR_HAS_DRM_BACKEND && WLR_HAS_LIBINPUT_BACKEND)) {
        wlr_log(WLR_ERROR, "wlroots needs session, DRM, and libinput support for --session");
        return 1;
    }
    const char *backends = mode == SH_BACKEND_SESSION    ? "drm,libinput"
                           : mode == SH_BACKEND_HEADLESS ? "headless"
                                                         : "wayland";
    if (setenv("WLR_BACKENDS", backends, 1) < 0)
        return 1;
    if (mode == SH_BACKEND_HEADLESS)
        setenv("WLR_HEADLESS_OUTPUTS", "1", 1);

    struct sh_server server = {.callbacks = callbacks};
    wl_list_init(&server.subscribers);
    server.tiling = sh_tiling_create();
    if (!server.tiling)
        return 1;
    server.tiling_enabled = server_settings(&server)->tiling;

    server.wl_display = wl_display_create();
    if (!server.wl_display)
        return 1;
    struct wl_event_loop *loop = wl_display_get_event_loop(server.wl_display);
    struct wl_event_source *sigint =
        wl_event_loop_add_signal(loop, SIGINT, terminate_signal, &server);
    struct wl_event_source *sigterm =
        wl_event_loop_add_signal(loop, SIGTERM, terminate_signal, &server);
    struct wl_event_source *sighup = wl_event_loop_add_signal(loop, SIGHUP, reload_signal, &server);
    struct wl_event_source *sigchld =
        wl_event_loop_add_signal(loop, SIGCHLD, reap_children, &server);

    server.backend = wlr_backend_autocreate(loop,
#if WLR_HAS_SESSION
                                            &server.session
#else
                                            NULL
#endif
    );
    if (server.backend == NULL) {
        wlr_log(WLR_ERROR, "failed to create wlr_backend");
        return 1;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (server.renderer == NULL) {
        wlr_log(WLR_ERROR, "failed to create wlr_renderer");
        return 1;
    }

    wlr_renderer_init_wl_shm(server.renderer, server.wl_display);
    // GPU clients (browsers, Electron, games) share buffers by dmabuf; the scene sends them
    // scanout feedback, and explicit sync keeps NVIDIA from showing unfinished frames.
    struct wlr_linux_dmabuf_v1 *linux_dmabuf = NULL;
    if (wlr_renderer_get_texture_formats(server.renderer, WLR_BUFFER_CAP_DMABUF))
        linux_dmabuf =
            wlr_linux_dmabuf_v1_create_with_renderer(server.wl_display, 4, server.renderer);
    int drm_fd = wlr_renderer_get_drm_fd(server.renderer);
    if (drm_fd >= 0 && server.renderer->features.timeline && server.backend->features.timeline)
        wlr_linux_drm_syncobj_manager_v1_create(server.wl_display, 1, drm_fd);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (server.allocator == NULL) {
        wlr_log(WLR_ERROR, "failed to create wlr_allocator");
        return 1;
    }

    struct wlr_compositor *compositor =
        wlr_compositor_create(server.wl_display, 5, server.renderer);
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);
    wlr_primary_selection_v1_device_manager_create(server.wl_display);
    wlr_data_control_manager_v1_create(server.wl_display);
    wlr_ext_data_control_manager_v1_create(server.wl_display, 1);
    wlr_viewporter_create(server.wl_display);
    wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
    wlr_single_pixel_buffer_manager_v1_create(server.wl_display);
    wlr_presentation_create(server.wl_display, server.backend, 2);
    wlr_xdg_wm_dialog_v1_create(server.wl_display, 1);
    // Portals parent their file choosers and share dialogs to the requesting window.
    struct wlr_xdg_foreign_registry *foreign_registry =
        wlr_xdg_foreign_registry_create(server.wl_display);
    wlr_xdg_foreign_v1_create(server.wl_display, foreign_registry);
    wlr_xdg_foreign_v2_create(server.wl_display, foreign_registry);

    server.output_layout = wlr_output_layout_create(server.wl_display);
    wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

    wl_list_init(&server.outputs);
    add_listener(&server.backend->events.new_output, &server.new_output, server_new_output);

    server.scene = wlr_scene_create();
    if (linux_dmabuf)
        wlr_scene_set_linux_dmabuf_v1(server.scene, linux_dmabuf);
    wlr_scene_set_gamma_control_manager_v1(server.scene,
                                           wlr_gamma_control_manager_v1_create(server.wl_display));
    // Stacking order, bottom to top.
    struct wlr_scene_tree **stack[] = {
        &server.backgrounds, &server.layer_trees[0], &server.layer_trees[1],
        &server.windows,     &server.layer_trees[2], &server.fullscreen,
        &server.unmanaged,   &server.layer_trees[3], &server.drag_icons,
        &server.lock_tree,
    };
    for (size_t i = 0; i < sizeof(stack) / sizeof(stack[0]); ++i)
        *stack[i] = wlr_scene_tree_create(&server.scene->tree);
    server.lock_blanks = wlr_scene_tree_create(server.lock_tree);
    wlr_scene_node_set_enabled(&server.lock_tree->node, false);
    struct wlr_session_lock_manager_v1 *lock_manager =
        wlr_session_lock_manager_v1_create(server.wl_display);
    add_listener(&lock_manager->events.new_lock, &server.new_lock, server_new_lock);
    wl_list_init(&server.layers);
    struct wlr_layer_shell_v1 *layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
    add_listener(&layer_shell->events.new_surface, &server.new_layer_surface,
                 server_new_layer_surface);
    server.foreign_manager = wlr_foreign_toplevel_manager_v1_create(server.wl_display);
    // Screen capture for screenshots and portal screen sharing (xdg-desktop-portal-wlr).
    wlr_screencopy_manager_v1_create(server.wl_display);
    wlr_export_dmabuf_manager_v1_create(server.wl_display);
    wlr_ext_image_copy_capture_manager_v1_create(server.wl_display, 1);
    wlr_ext_output_image_capture_source_manager_v1_create(server.wl_display, 1);
    server.toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(server.wl_display, 1);
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *toplevel_capture =
        wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(server.wl_display, 1);
    add_listener(&toplevel_capture->events.new_request, &server.new_capture_request,
                 server_new_capture_request);
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    wl_list_init(&server.toplevels);
    struct wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
    add_listener(&xdg_shell->events.new_toplevel, &server.new_xdg_toplevel,
                 server_new_xdg_toplevel);
    add_listener(&xdg_shell->events.new_popup, &server.new_xdg_popup, server_new_xdg_popup);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    add_listener(&server.cursor->events.motion, &server.cursor_motion, server_cursor_motion);
    add_listener(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute,
                 server_cursor_motion_absolute);
    add_listener(&server.cursor->events.button, &server.cursor_button, server_cursor_button);
    add_listener(&server.cursor->events.axis, &server.cursor_axis, server_cursor_axis);
    add_listener(&server.cursor->events.frame, &server.cursor_frame, server_cursor_frame);

    wl_list_init(&server.keyboards);
    add_listener(&server.backend->events.new_input, &server.new_input, server_new_input);
    server.seat = wlr_seat_create(server.wl_display, "seat0");
    add_listener(&server.seat->events.request_set_cursor, &server.request_cursor,
                 seat_request_cursor);
    struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr =
        wlr_cursor_shape_manager_v1_create(server.wl_display, 1);
    add_listener(&cursor_shape_mgr->events.request_set_shape, &server.request_set_shape,
                 cursor_request_set_shape);
    add_listener(&server.seat->pointer_state.events.focus_change, &server.pointer_focus_change,
                 seat_pointer_focus_change);
    add_listener(&server.seat->events.request_set_selection, &server.request_set_selection,
                 seat_request_set_selection);
    add_listener(&server.seat->events.request_set_primary_selection,
                 &server.request_set_primary_selection, seat_request_set_primary_selection);
    add_listener(&server.seat->events.request_start_drag, &server.request_start_drag,
                 seat_request_start_drag);
    add_listener(&server.seat->events.start_drag, &server.start_drag, seat_start_drag);
    add_listener(&server.seat->keyboard_state.events.focus_change, &server.keyboard_focus_change,
                 seat_keyboard_focus_change);
    struct wlr_xdg_activation_v1 *activation = wlr_xdg_activation_v1_create(server.wl_display);
    add_listener(&activation->events.request_activate, &server.request_activate, request_activate);
    server.relative_pointer = wlr_relative_pointer_manager_v1_create(server.wl_display);
    server.constraints = wlr_pointer_constraints_v1_create(server.wl_display);
    add_listener(&server.constraints->events.new_constraint, &server.new_constraint,
                 server_new_constraint);
    server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);
    struct wlr_idle_inhibit_manager_v1 *idle_inhibit =
        wlr_idle_inhibit_v1_create(server.wl_display);
    add_listener(&idle_inhibit->events.new_inhibitor, &server.new_inhibitor, server_new_inhibitor);

    const char *socket = wl_display_add_socket_auto(server.wl_display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        return 1;
    }

    if (!wlr_backend_start(server.backend)) {
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.wl_display);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    open_control_socket(&server, socket);
    setenv("XDG_CURRENT_DESKTOP", "shaoDe", true);
    setenv("XDG_SESSION_TYPE", "wayland", true);
    // Firefox, Electron (Discord, VS Code), and Java would otherwise need to be told to use
    // Wayland or to cope without a reparenting window manager. The user's own values win.
    setenv("MOZ_ENABLE_WAYLAND", "1", false);
    setenv("ELECTRON_OZONE_PLATFORM_HINT", "auto", false);
    setenv("_JAVA_AWT_WM_NONREPARENTING", "1", false);
    unsetenv("DISPLAY");
#if WLR_HAS_XWAYLAND
    // Xwayland starts when the first X11 client connects and exits once idle.
    if (server_settings(&server)->xwayland) {
        server.xwayland = wlr_xwayland_create(server.wl_display, compositor, true);
        if (server.xwayland) {
            add_listener(&server.xwayland->events.ready, &server.xwayland_ready, xwayland_ready);
            add_listener(&server.xwayland->events.new_surface, &server.new_xwayland_surface,
                         server_new_xwayland_surface);
            setenv("DISPLAY", server.xwayland->display_name, true);
            wlr_log(WLR_INFO, "XWayland listening on DISPLAY=%s", server.xwayland->display_name);
        } else {
            wlr_log(WLR_ERROR, "Cannot create XWayland; X11 applications are unavailable");
        }
    }
#else
    (void)compositor;
#endif
    server.running = true;
    callbacks->startup(callbacks->userdata);

    wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.wl_display);
    server.running = false;
    wl_event_source_remove(sigint);
    wl_event_source_remove(sigterm);
    wl_event_source_remove(sighup);
    wl_event_source_remove(sigchld);

#if WLR_HAS_XWAYLAND
#if SHAODE_XWM_WAKER
    close_xwm_waker(&server);
#endif
    if (server.xwayland) {
        wl_list_remove(&server.xwayland_ready.link);
        wl_list_remove(&server.new_xwayland_surface.link);
        wlr_xwayland_destroy(server.xwayland);
    }
#endif
    close_control_socket(&server);
    wl_display_destroy_clients(server.wl_display);

    wl_list_remove(&server.new_xdg_toplevel.link);
    wl_list_remove(&server.new_xdg_popup.link);
    wl_list_remove(&server.new_layer_surface.link);

    wl_list_remove(&server.cursor_motion.link);
    wl_list_remove(&server.cursor_motion_absolute.link);
    wl_list_remove(&server.cursor_button.link);
    wl_list_remove(&server.cursor_axis.link);
    wl_list_remove(&server.cursor_frame.link);

    wl_list_remove(&server.new_input.link);
    wl_list_remove(&server.request_cursor.link);
    wl_list_remove(&server.request_set_shape.link);
    wl_list_remove(&server.pointer_focus_change.link);
    wl_list_remove(&server.request_set_selection.link);
    wl_list_remove(&server.request_set_primary_selection.link);
    wl_list_remove(&server.request_start_drag.link);
    wl_list_remove(&server.start_drag.link);
    wl_list_remove(&server.keyboard_focus_change.link);
    wl_list_remove(&server.request_activate.link);
    wl_list_remove(&server.new_constraint.link);
    wl_list_remove(&server.new_capture_request.link);

    wl_list_remove(&server.new_output.link);
    wl_list_remove(&server.new_lock.link);
    wl_list_remove(&server.new_inhibitor.link);

    wlr_backend_destroy(server.backend);
    wlr_scene_node_destroy(&server.scene->tree.node);
    for (int i = 0; i < 2; ++i)
        wlr_buffer_drop(server.deco_buffers[i]);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wl_display_destroy(server.wl_display);
    sh_tiling_destroy(server.tiling);
    return 0;
}
