/* Derived from wlroots TinyWL 0.20.2; see vendor/tinywl/LICENSE. */
#include "shaode/backend.h"
#include <assert.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>
#if WLR_HAS_XWAYLAND
#include <wlr/xwayland.h>
#endif
#include <xkbcommon/xkbcommon.h>

_Static_assert((unsigned)SH_ALT == (unsigned)WLR_MODIFIER_ALT &&
                   (unsigned)SH_SHIFT == (unsigned)WLR_MODIFIER_SHIFT &&
                   (unsigned)SH_CTRL == (unsigned)WLR_MODIFIER_CTRL &&
                   (unsigned)SH_LOGO == (unsigned)WLR_MODIFIER_LOGO,
               "C++ configuration and wlroots modifier bits must agree");

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
    struct wl_event_source *startup_timeout;
    xcb_connection_t *xwm_waker;
    xcb_atom_t waker_atom;
    xcb_window_t xwm_window;
    struct wl_event_source *waker_timer, *waker_input;
#endif
    bool started;
    struct wlr_scene_tree *layer_trees[4];
    struct wl_list layers;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct sh_layer *focused_layer;
    struct sh_toplevel *focused_toplevel;
    struct wlr_foreign_toplevel_manager_v1 *foreign_manager;

    /* Session lock: `locked` outlives a crashed locker so the screen stays covered. */
    bool locked;
    struct sh_lock *lock;
    struct wlr_scene_tree *lock_tree, *lock_blanks;
    struct wlr_session_lock_manager_v1 *lock_manager;
    struct wl_listener new_lock;
    struct wlr_idle_notifier_v1 *idle_notifier;
    struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
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

    struct wlr_xdg_shell *xdg_shell;
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
    struct wl_listener pointer_focus_change;
    struct wl_listener request_set_selection;
    struct wl_list keyboards;
    enum sh_cursor_mode cursor_mode;
    struct sh_toplevel *grabbed_toplevel;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

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
    struct wlr_foreign_toplevel_handle_v1 *foreign;
    struct wl_listener title_changed, app_id_changed;
    struct wl_listener foreign_activate, foreign_close, foreign_maximize, foreign_minimize;
    struct wl_listener foreign_fullscreen;
    bool fullscreen;
    struct wlr_box fullscreen_restore;
    struct wl_listener request_minimize;
    struct wlr_box restore_box;
    bool arranged;
    struct wl_list link;
    struct sh_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel; // NULL for X11 windows
#if WLR_HAS_XWAYLAND
    struct wlr_xwayland_surface *xsurface; // NULL for xdg-shell windows
    bool unmanaged, associated;            // unmanaged: override-redirect menus and tooltips
    struct wl_listener x_associate, x_dissociate, x_configure, x_activate, x_geometry;
#endif
    struct wlr_scene_tree *scene_tree;
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
static void toplevel_set_position(struct sh_toplevel *toplevel, int x, int y) {
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
#if WLR_HAS_XWAYLAND
    if (toplevel->xsurface && toplevel->xsurface->width > 0 && toplevel->xsurface->height > 0)
        wlr_xwayland_surface_configure(toplevel->xsurface, x, y, toplevel->xsurface->width,
                                       toplevel->xsurface->height);
#endif
}
static void toplevel_set_states(struct sh_toplevel *toplevel, bool maximized, uint32_t tiled) {
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
static void reflow_output(struct sh_server *server, struct wlr_output *output);
static void create_popup(struct wlr_xdg_popup *popup, struct wlr_scene_tree *parent);

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
    deactivate_toplevel(server);
    server->focused_layer = NULL;
    server->focused_toplevel = toplevel;
    toplevel->minimized = false;
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
        if (!toplevel->minimized) {
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
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_minimized(toplevel->foreign, true);
    if (toplevel->server->focused_toplevel == toplevel)
        focus_previous(toplevel->server);
}

static void usable_area(struct sh_server *server, struct wlr_output *output, struct wlr_box *box) {
    struct sh_output *candidate;
    wl_list_for_each(candidate, &server->outputs, link) {
        if (candidate->wlr_output == output) {
            *box = candidate->usable;
            return;
        }
    }
    wlr_output_layout_get_box(server->output_layout, output, box);
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    struct sh_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);

    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);

    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->wlr_keyboard->modifiers);
}

static void reload_config(struct sh_server *server);
static void set_fullscreen(struct sh_toplevel *toplevel, bool fullscreen);
static void arrange_windows(struct sh_server *server, enum sh_action action);
static void begin_interactive(struct sh_toplevel *toplevel, enum sh_cursor_mode mode,
                              uint32_t edges);

static bool handle_keybinding(struct sh_server *server, uint32_t modifiers, xkb_keysym_t sym) {
#if WLR_HAS_SESSION
    if (server->session && sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
        wlr_session_change_vt(server->session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
        return true;
    }
#endif
    if (server->locked)
        return false; // Every other key belongs to the lock screen.
    enum sh_action action = server->callbacks->key(server->callbacks->userdata, modifiers, sym);
    switch (action) {
    case SH_NONE:
        return false;
    case SH_HANDLED:
        break;
    case SH_QUIT:
        wl_display_terminate(server->wl_display);
        break;
    case SH_RELOAD:
        reload_config(server);
        break;
    case SH_CYCLE:
        if (wl_list_length(&server->toplevels) > 1) {
            struct sh_toplevel *next = wl_container_of(server->toplevels.prev, next, link);
            focus_toplevel(next);
        }
        break;
    case SH_FULLSCREEN:
        if (!wl_list_empty(&server->toplevels)) {
            struct sh_toplevel *focused = wl_container_of(server->toplevels.next, focused, link);
            set_fullscreen(focused, !focused->fullscreen);
        }
        break;
    case SH_CLOSE:
        if (!wl_list_empty(&server->toplevels)) {
            struct sh_toplevel *focused = wl_container_of(server->toplevels.next, focused, link);
            toplevel_close(focused);
        }
        break;
    default:
        arrange_windows(server, action);
        break;
    }
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
    const struct sh_settings *settings = server->callbacks->settings(server->callbacks->userdata);
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

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

    wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct sh_server *server, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(server, device);
        break;
    default:
        break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, request_cursor);

    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;

    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, pointer_focus_change);

    struct wlr_seat_pointer_focus_change_event *event = data;
    if (event->new_surface == NULL) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
    }
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
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

static void reset_cursor_mode(struct sh_server *server) {
    server->cursor_mode = SH_CURSOR_PASSTHROUGH;
    server->grabbed_toplevel = NULL;
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

    struct wlr_box geo_box = toplevel_geometry(toplevel);
    toplevel_configure(toplevel, new_left - geo_box.x, new_top - geo_box.y, new_right - new_left,
                       new_bottom - new_top);
}

static void process_cursor_motion(struct sh_server *server, uint32_t time) {
    if (server->cursor_mode == SH_CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    } else if (server->cursor_mode == SH_CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    desktop_toplevel_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (!surface) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
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
    if (server->grab_button == event->button && event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        server->grab_button = 0;
        reset_cursor_mode(server);
        process_cursor_motion(server, event->time_msec);
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
        const struct sh_settings *settings =
            server->callbacks->settings(server->callbacks->userdata);
        if (toplevel && (mods & settings->mouse_modifier) &&
            (event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
            server->grab_button = event->button;
            begin_interactive(toplevel,
                              event->button == BTN_LEFT ? SH_CURSOR_MOVE : SH_CURSOR_RESIZE,
                              WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
            return;
        }
    }
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED)
        reset_cursor_mode(server);
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

static void lock_output_presented(struct sh_output *output);

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
    const struct sh_settings *settings = server->callbacks->settings(server->callbacks->userdata);
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
    lock_surface->map.notify = lock_surface_map;
    wl_signal_add(&surface->surface->events.map, &lock_surface->map);
    lock_surface->destroy.notify = lock_surface_destroy;
    wl_signal_add(&surface->events.destroy, &lock_surface->destroy);
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
    lock->new_surface.notify = lock_new_surface;
    wl_signal_add(&wlr_lock->events.new_surface, &lock->new_surface);
    lock->unlock.notify = lock_unlock;
    wl_signal_add(&wlr_lock->events.unlock, &lock->unlock);
    lock->destroy.notify = lock_destroy;
    wl_signal_add(&wlr_lock->events.destroy, &lock->destroy);
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
    inhibitor->destroy.notify = inhibitor_destroy;
    wl_signal_add(&wlr_inhibitor->events.destroy, &inhibitor->destroy);
    wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, ++server->inhibitors > 0);
}

static void reload_config(struct sh_server *server) {
    if (!server->callbacks->reload(server->callbacks->userdata))
        return;
    struct sh_keyboard *keyboard;
    wl_list_for_each(keyboard, &server->keyboards, link) {
        if (!configure_keyboard(server, keyboard->wlr_keyboard))
            wlr_log(WLR_ERROR, "Could not apply reloaded keymap");
    }
    update_backgrounds(server);
}

static void refit_fullscreen(struct sh_server *server);

static void output_request_state(struct wl_listener *listener, void *data) {
    struct sh_output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    if (wlr_output_commit_state(output->wlr_output, event->state)) {
        update_backgrounds(output->server);
        arrange_layers(output->server);
        refit_fullscreen(output->server);
    }
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
    if (server->running && wl_list_empty(&server->outputs))
        wl_display_terminate(server->wl_display);
    free(output);
    send_locked_if_presented(server);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    }

    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct sh_output *output = calloc(1, sizeof(*output));
    output->wlr_output = wlr_output;
    output->server = server;
    output->background =
        wlr_scene_rect_create(server->backgrounds, 1, 1,
                              server->callbacks->settings(server->callbacks->userdata)->background);
    static const float lock_color[4] = {0, 0, 0, 1};
    output->lock_blank = wlr_scene_rect_create(server->lock_blanks, 1, 1, lock_color);
    // An output added while locked must not show the desktop, even briefly.
    output->lock_presented = false;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    struct wlr_output_layout_output *l_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
    if (wlr_output_is_wl(wlr_output))
        wlr_wl_output_set_title(wlr_output, "shaoDe — nested desktop");
    update_backgrounds(server);
    arrange_layers(server);
}

/* Preserve the original floating rectangle across repeated snap operations. */
static void place_toplevel(struct sh_toplevel *toplevel, enum sh_action action,
                           struct sh_rect target) {
    struct wlr_box geometry = toplevel_geometry(toplevel);
    if (!toplevel->arranged) {
        toplevel->restore_box =
            (struct wlr_box){toplevel->scene_tree->node.x, toplevel->scene_tree->node.y,
                             geometry.width, geometry.height};
    }
    toplevel->arranged = true;
    toplevel->arrangement = action;
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel->foreign, action == SH_MAXIMIZE);
    toplevel_set_states(toplevel, action == SH_MAXIMIZE,
                        action == SH_MAXIMIZE
                            ? 0
                            : WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
    toplevel_configure(toplevel, target.x, target.y, target.width, target.height);
}

static void restore_toplevel(struct sh_toplevel *toplevel) {
    if (!toplevel->arranged)
        return;
    toplevel->arranged = false;
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel->foreign, false);
    toplevel_set_states(toplevel, false, 0);
    toplevel_configure(toplevel, toplevel->restore_box.x, toplevel->restore_box.y,
                       toplevel->restore_box.width, toplevel->restore_box.height);
}

static struct wlr_output *toplevel_output(struct sh_toplevel *toplevel) {
    struct sh_server *server = toplevel->server;
    struct wlr_output *output = wlr_output_layout_output_at(
        server->output_layout, toplevel->scene_tree->node.x, toplevel->scene_tree->node.y);
    if (!output && !wl_list_empty(&server->outputs)) {
        struct sh_output *first = wl_container_of(server->outputs.next, first, link);
        output = first->wlr_output;
    }
    return output;
}

static void arrange_windows(struct sh_server *server, enum sh_action action) {
    if (wl_list_empty(&server->toplevels))
        return;
    struct sh_toplevel *focused = wl_container_of(server->toplevels.next, focused, link);
    if (focused->fullscreen)
        set_fullscreen(focused, false);
    if (action == SH_RESTORE) {
        restore_toplevel(focused);
        return;
    }
    struct wlr_output *output = toplevel_output(focused);
    if (!output)
        return;
    struct wlr_box box;
    usable_area(server, output, &box);
    struct sh_rect area = {box.x, box.y, box.width, box.height}, target;
    int gap = server->callbacks->settings(server->callbacks->userdata)->gap;
    if (action != SH_TILE) {
        if (sh_placement(action, area, gap, 0, 1, &target))
            place_toplevel(focused, action, target);
        return;
    }
    int count = 0, index = 0;
    struct sh_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel_output(toplevel) == output && !toplevel->minimized && !toplevel->fullscreen)
            ++count;
    }
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel_output(toplevel) != output || toplevel->minimized || toplevel->fullscreen)
            continue;
        if (sh_placement(action, area, gap, index++, count, &target))
            place_toplevel(toplevel, action, target);
    }
}

static void reflow_output(struct sh_server *server, struct wlr_output *output) {
    struct wlr_box box;
    usable_area(server, output, &box);
    struct sh_rect area = {box.x, box.y, box.width, box.height}, target;
    int count = 0, index = 0;
    int gap = server->callbacks->settings(server->callbacks->userdata)->gap;
    struct sh_toplevel *toplevel;
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel->arranged && !toplevel->minimized && !toplevel->fullscreen &&
            toplevel_output(toplevel) == output && toplevel->arrangement == SH_TILE)
            ++count;
    }
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (!toplevel->arranged || toplevel->minimized || toplevel->fullscreen ||
            toplevel_output(toplevel) != output)
            continue;
        enum sh_action action = toplevel->arrangement;
        if (sh_placement(action, area, gap, action == SH_TILE ? index++ : 0,
                         action == SH_TILE ? count : 1, &target))
            place_toplevel(toplevel, action, target);
    }
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
    struct wlr_output *output = toplevel_output(toplevel);
    if (!output)
        return;
    struct wlr_box box;
    usable_area(toplevel->server, output, &box);
    place_toplevel(toplevel, SH_MAXIMIZE, (struct sh_rect){box.x, box.y, box.width, box.height});
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
static void toplevel_title_changed(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, title_changed);
    const char *title = toplevel_title(toplevel);
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_title(toplevel->foreign, title ? title : "Untitled");
}
static void toplevel_app_id_changed(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, app_id_changed);
    const char *app_id = toplevel_app_id(toplevel);
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_app_id(toplevel->foreign, app_id ? app_id : "");
}
static void publish_toplevel(struct sh_toplevel *toplevel) {
    toplevel->foreign = wlr_foreign_toplevel_handle_v1_create(toplevel->server->foreign_manager);
    if (!toplevel->foreign)
        return;
    toplevel_title_changed(&toplevel->title_changed, NULL);
    toplevel_app_id_changed(&toplevel->app_id_changed, NULL);
    toplevel->foreign_activate.notify = foreign_activate;
    wl_signal_add(&toplevel->foreign->events.request_activate, &toplevel->foreign_activate);
    toplevel->foreign_close.notify = foreign_close;
    wl_signal_add(&toplevel->foreign->events.request_close, &toplevel->foreign_close);
    toplevel->foreign_maximize.notify = foreign_maximize;
    wl_signal_add(&toplevel->foreign->events.request_maximize, &toplevel->foreign_maximize);
    toplevel->foreign_minimize.notify = foreign_minimize;
    wl_signal_add(&toplevel->foreign->events.request_minimize, &toplevel->foreign_minimize);
    toplevel->foreign_fullscreen.notify = foreign_fullscreen;
    wl_signal_add(&toplevel->foreign->events.request_fullscreen, &toplevel->foreign_fullscreen);
    wlr_foreign_toplevel_handle_v1_set_fullscreen(toplevel->foreign, toplevel->fullscreen);
    struct wlr_output *output = toplevel_output(toplevel);
    if (output)
        wlr_foreign_toplevel_handle_v1_output_enter(toplevel->foreign, output);
}
static void unpublish_toplevel(struct sh_toplevel *toplevel) {
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
    create_popup(data, layer->scene->tree);
}
static void server_new_layer_surface(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *surface = data;
    if (!surface->output) {
        if (wl_list_empty(&server->outputs)) {
            wlr_layer_surface_v1_destroy(surface);
            return;
        }
        struct sh_output *first = wl_container_of(server->outputs.next, first, link);
        surface->output = first->wlr_output;
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
    layer->commit.notify = layer_commit;
    wl_signal_add(&surface->surface->events.commit, &layer->commit);
    layer->map.notify = layer_map;
    wl_signal_add(&surface->surface->events.map, &layer->map);
    layer->unmap.notify = layer_unmap;
    wl_signal_add(&surface->surface->events.unmap, &layer->unmap);
    layer->destroy.notify = layer_destroy;
    wl_signal_add(&surface->events.destroy, &layer->destroy);
    layer->new_popup.notify = layer_new_popup;
    wl_signal_add(&surface->events.new_popup, &layer->new_popup);
}

static void maximize_toplevel(struct sh_toplevel *toplevel, bool maximized) {
    if (!maximized) {
        restore_toplevel(toplevel);
        return;
    }
    struct wlr_output *output = toplevel_output(toplevel);
    if (!output)
        return;
    struct wlr_box box;
    usable_area(toplevel->server, output, &box);
    place_toplevel(toplevel, SH_MAXIMIZE, (struct sh_rect){box.x, box.y, box.width, box.height});
}

static void map_toplevel(struct sh_toplevel *toplevel, bool fullscreen, bool maximized) {
    int offset = 40 + 32 * (wl_list_length(&toplevel->server->toplevels) % 8);
    int x = offset, y = offset;
    bool resize = false;
    struct wlr_box geometry = toplevel_geometry(toplevel);
    int width = geometry.width, height = geometry.height;
    struct wlr_output *output = toplevel_output(toplevel);
    if (output) {
        struct wlr_box area;
        usable_area(toplevel->server, output, &area);
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

    toplevel->fullscreen = false;
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

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
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

static void begin_interactive(struct sh_toplevel *toplevel, enum sh_cursor_mode mode,
                              uint32_t edges) {
    struct sh_server *server = toplevel->server;
    if (toplevel->fullscreen)
        return;

    toplevel->arranged = false;
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel->foreign, false);
    toplevel_set_states(toplevel, false, 0);
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == SH_CURSOR_MOVE) {
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        struct wlr_box geo_box = toplevel_geometry(toplevel);

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

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    struct wlr_xdg_toplevel_move_event *event = data;
    if (wlr_seat_validate_pointer_grab_serial(toplevel->server->seat,
                                              toplevel->xdg_toplevel->base->surface, event->serial))
        begin_interactive(toplevel, SH_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    if (wlr_seat_validate_pointer_grab_serial(toplevel->server->seat,
                                              toplevel->xdg_toplevel->base->surface, event->serial))
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
    toplevel_configure(toplevel, box.x, box.y, box.width, box.height);
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
    struct wlr_box geometry = toplevel_geometry(toplevel);
    if (fullscreen)
        toplevel->fullscreen_restore =
            (struct wlr_box){toplevel->scene_tree->node.x, toplevel->scene_tree->node.y,
                             geometry.width, geometry.height};
    toplevel->fullscreen = fullscreen;
    toplevel_set_fullscreen_state(toplevel, fullscreen);
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_fullscreen(toplevel->foreign, fullscreen);
    if (fullscreen) {
        fit_fullscreen(toplevel);
    } else {
        toplevel_configure(toplevel, toplevel->fullscreen_restore.x, toplevel->fullscreen_restore.y,
                           toplevel->fullscreen_restore.width, toplevel->fullscreen_restore.height);
        wlr_scene_node_reparent(&toplevel->scene_tree->node, server->windows);
        // The usable area may have changed while this window covered the output.
        struct wlr_output *output = toplevel_output(toplevel);
        if (toplevel->arranged && output)
            reflow_output(server, output);
    }
    if (server->focused_toplevel == toplevel || fullscreen)
        focus_toplevel(toplevel);
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

    toplevel->title_changed.notify = toplevel_title_changed;
    wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->title_changed);
    toplevel->app_id_changed.notify = toplevel_app_id_changed;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->app_id_changed);
    toplevel->request_minimize.notify = toplevel_request_minimize;
    wl_signal_add(&xdg_toplevel->events.request_minimize, &toplevel->request_minimize);
    toplevel->map.notify = xdg_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

    toplevel->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
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
    toplevel->map.notify = xwayland_map;
    wl_signal_add(&toplevel->xsurface->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = xwayland_unmap;
    wl_signal_add(&toplevel->xsurface->surface->events.unmap, &toplevel->unmap);
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

static bool xwayland_managed(struct sh_toplevel *toplevel) {
    return toplevel_mapped(toplevel) && !toplevel->unmanaged;
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
    if (toplevel->fullscreen || toplevel->arranged) {
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
    struct {
        struct wl_listener *listener;
        struct wl_signal *signal;
        wl_notify_func_t notify;
    } listeners[] = {
        {&toplevel->x_associate, &xsurface->events.associate, xwayland_associate},
        {&toplevel->x_dissociate, &xsurface->events.dissociate, xwayland_dissociate},
        {&toplevel->destroy, &xsurface->events.destroy, xwayland_destroy},
        {&toplevel->x_configure, &xsurface->events.request_configure, xwayland_request_configure},
        {&toplevel->x_activate, &xsurface->events.request_activate, xwayland_request_activate},
        {&toplevel->x_geometry, &xsurface->events.set_geometry, xwayland_set_geometry},
        {&toplevel->request_move, &xsurface->events.request_move, xwayland_request_move},
        {&toplevel->request_resize, &xsurface->events.request_resize, xwayland_request_resize},
        {&toplevel->request_maximize, &xsurface->events.request_maximize,
         xwayland_request_maximize},
        {&toplevel->request_fullscreen, &xsurface->events.request_fullscreen,
         xwayland_request_fullscreen},
        {&toplevel->request_minimize, &xsurface->events.request_minimize,
         xwayland_request_minimize},
        {&toplevel->title_changed, &xsurface->events.set_title, toplevel_title_changed},
        {&toplevel->app_id_changed, &xsurface->events.set_class, toplevel_app_id_changed},
    };
    for (size_t i = 0; i < sizeof(listeners) / sizeof(listeners[0]); ++i) {
        listeners[i].listener->notify = listeners[i].notify;
        wl_signal_add(listeners[i].signal, listeners[i].listener);
    }
}

static void run_startup(struct sh_server *server);

/* wlroots' XWM can strand X events: xcb_flush() in its write-only wakeup reads
 * pending input into xcb's queue, and nothing processes that queue until more
 * data arrives. A periodic client message from a separate connection, sent only
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
    wl_event_source_timer_update(server->waker_timer, XWM_WAKE_INTERVAL_MS);
}

static void xwayland_ready(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, xwayland_ready);
    wlr_log(WLR_INFO, "XWayland ready on DISPLAY=%s", server->xwayland->display_name);
    wlr_xwayland_set_seat(server->xwayland, server->seat);
    open_xwm_waker(server);
    if (wlr_xcursor_manager_load(server->cursor_mgr, 1)) {
        struct wlr_xcursor *xcursor =
            wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "default", 1);
        if (xcursor) {
            struct wlr_xcursor_image *image = xcursor->images[0];
            wlr_xwayland_set_cursor(server->xwayland, wlr_xcursor_image_get_buffer(image),
                                    image->hotspot_x, image->hotspot_y);
        }
    }
    run_startup(server);
}

static int startup_timeout(void *data) {
    wlr_log(WLR_ERROR, "XWayland did not become ready; starting applications anyway");
    run_startup(data);
    return 0;
}
#endif

/* Launch the shell and startup commands once X11 clients can be managed: the
 * window manager ignores windows created before it attaches to Xwayland. */
static void run_startup(struct sh_server *server) {
    if (server->started)
        return;
    server->started = true;
#if WLR_HAS_XWAYLAND
    if (server->startup_timeout) {
        wl_event_source_remove(server->startup_timeout);
        server->startup_timeout = NULL;
    }
#endif
    server->callbacks->startup(server->callbacks->userdata);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    struct sh_popup *popup = wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    struct sh_popup *popup = wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);

    free(popup);
}

static void create_popup(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree) {
    struct sh_popup *popup = calloc(1, sizeof(*popup));
    popup->xdg_popup = xdg_popup;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *popup = data;
    // A layer-shell popup is attached by the layer's new_popup handler instead.
    if (!popup->parent)
        return;
    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (parent && parent->data)
        create_popup(popup, parent->data);
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

    wlr_renderer_init_wl_display(server.renderer, server.wl_display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (server.allocator == NULL) {
        wlr_log(WLR_ERROR, "failed to create wlr_allocator");
        return 1;
    }

    struct wlr_compositor *compositor =
        wlr_compositor_create(server.wl_display, 5, server.renderer);
    wlr_subcompositor_create(server.wl_display);
    wlr_data_device_manager_create(server.wl_display);

    server.output_layout = wlr_output_layout_create(server.wl_display);

    wl_list_init(&server.outputs);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.scene = wlr_scene_create();
    server.backgrounds = wlr_scene_tree_create(&server.scene->tree);
    server.layer_trees[0] = wlr_scene_tree_create(&server.scene->tree);
    server.layer_trees[1] = wlr_scene_tree_create(&server.scene->tree);
    server.windows = wlr_scene_tree_create(&server.scene->tree);
    server.layer_trees[2] = wlr_scene_tree_create(&server.scene->tree);
    server.fullscreen = wlr_scene_tree_create(&server.scene->tree);
    server.unmanaged = wlr_scene_tree_create(&server.scene->tree);
    server.layer_trees[3] = wlr_scene_tree_create(&server.scene->tree);
    server.lock_tree = wlr_scene_tree_create(&server.scene->tree);
    server.lock_blanks = wlr_scene_tree_create(server.lock_tree);
    wlr_scene_node_set_enabled(&server.lock_tree->node, false);
    server.lock_manager = wlr_session_lock_manager_v1_create(server.wl_display);
    server.new_lock.notify = server_new_lock;
    wl_signal_add(&server.lock_manager->events.new_lock, &server.new_lock);
    wl_list_init(&server.layers);
    server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
    server.new_layer_surface.notify = server_new_layer_surface;
    wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);
    server.foreign_manager = wlr_foreign_toplevel_manager_v1_create(server.wl_display);
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    wl_list_init(&server.toplevels);
    server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
    server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    server.cursor_mode = SH_CURSOR_PASSTHROUGH;
    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    wl_list_init(&server.keyboards);
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.seat = wlr_seat_create(server.wl_display, "seat0");
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.pointer_focus_change.notify = seat_pointer_focus_change;
    wl_signal_add(&server.seat->pointer_state.events.focus_change, &server.pointer_focus_change);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);
    server.idle_notifier = wlr_idle_notifier_v1_create(server.wl_display);
    server.idle_inhibit = wlr_idle_inhibit_v1_create(server.wl_display);
    server.new_inhibitor.notify = server_new_inhibitor;
    wl_signal_add(&server.idle_inhibit->events.new_inhibitor, &server.new_inhibitor);

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
    setenv("XDG_CURRENT_DESKTOP", "shaoDe", true);
    setenv("XDG_SESSION_TYPE", "wayland", true);
    unsetenv("DISPLAY");
#if WLR_HAS_XWAYLAND
    // Started eagerly: lazy startup races the first client against the window manager.
    if (callbacks->settings(callbacks->userdata)->xwayland) {
        server.xwayland = wlr_xwayland_create(server.wl_display, compositor, false);
        if (server.xwayland) {
            server.xwayland_ready.notify = xwayland_ready;
            wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);
            server.new_xwayland_surface.notify = server_new_xwayland_surface;
            wl_signal_add(&server.xwayland->events.new_surface, &server.new_xwayland_surface);
            setenv("DISPLAY", server.xwayland->display_name, true);
            wlr_log(WLR_INFO, "Starting XWayland on DISPLAY=%s", server.xwayland->display_name);
            server.startup_timeout = wl_event_loop_add_timer(loop, startup_timeout, &server);
            if (server.startup_timeout)
                wl_event_source_timer_update(server.startup_timeout, 10000);
        } else {
            wlr_log(WLR_ERROR, "Cannot create XWayland; X11 applications are unavailable");
        }
    }
#else
    (void)compositor;
#endif
    server.running = true;
#if WLR_HAS_XWAYLAND
    if (!server.xwayland)
#endif
        run_startup(&server);

    wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.wl_display);
    server.running = false;
    wl_event_source_remove(sigint);
    wl_event_source_remove(sigterm);
    wl_event_source_remove(sighup);
    wl_event_source_remove(sigchld);

#if WLR_HAS_XWAYLAND
    if (server.startup_timeout)
        wl_event_source_remove(server.startup_timeout);
    close_xwm_waker(&server);
    if (server.xwayland) {
        wl_list_remove(&server.xwayland_ready.link);
        wl_list_remove(&server.new_xwayland_surface.link);
        wlr_xwayland_destroy(server.xwayland);
    }
#endif
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
    wl_list_remove(&server.pointer_focus_change.link);
    wl_list_remove(&server.request_set_selection.link);

    wl_list_remove(&server.new_output.link);
    wl_list_remove(&server.new_lock.link);
    wl_list_remove(&server.new_inhibitor.link);

    wlr_backend_destroy(server.backend);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wl_display_destroy(server.wl_display);
    return 0;
}
