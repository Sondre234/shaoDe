/* Derived from wlroots TinyWL 0.20.2; see vendor/tinywl/LICENSE. */
#include "shaode/backend.h"
#include <assert.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
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
    struct wlr_scene_tree *layer_trees[4];
    struct wl_list layers;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wl_listener new_layer_surface;
    struct sh_layer *focused_layer;
    struct sh_toplevel *focused_toplevel;
    struct wlr_foreign_toplevel_manager_v1 *foreign_manager;
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
    struct wlr_scene_rect *background;
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
    struct wl_listener request_minimize;
    struct wlr_box restore_box;
    bool arranged;
    struct wl_list link;
    struct sh_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
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
static void reflow_output(struct sh_server *server, struct wlr_output *output);
static void create_popup(struct wlr_xdg_popup *popup, struct wlr_scene_tree *parent);

static void deactivate_toplevel(struct sh_server *server) {
    if (!server->focused_toplevel)
        return;
    struct sh_toplevel *old = server->focused_toplevel;
    wlr_xdg_toplevel_set_activated(old->xdg_toplevel, false);
    if (old->foreign)
        wlr_foreign_toplevel_handle_v1_set_activated(old->foreign, false);
    server->focused_toplevel = NULL;
}

static void focus_toplevel(struct sh_toplevel *toplevel) {
    if (!toplevel)
        return;
    struct sh_server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;
    deactivate_toplevel(server);
    server->focused_layer = NULL;
    server->focused_toplevel = toplevel;
    toplevel->minimized = false;
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
    if (toplevel->foreign) {
        wlr_foreign_toplevel_handle_v1_set_minimized(toplevel->foreign, false);
        wlr_foreign_toplevel_handle_v1_set_activated(toplevel->foreign, true);
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard)
        wlr_seat_keyboard_notify_enter(seat, toplevel->xdg_toplevel->base->surface,
                                       keyboard->keycodes, keyboard->num_keycodes,
                                       &keyboard->modifiers);
}

static void focus_previous(struct sh_server *server) {
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
    if (layer->surface->current.keyboard_interactive ==
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
    case SH_CLOSE:
        if (!wl_list_empty(&server->toplevels)) {
            struct sh_toplevel *focused = wl_container_of(server->toplevels.next, focused, link);
            wlr_xdg_toplevel_send_close(focused->xdg_toplevel);
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
    wlr_scene_node_set_position(&toplevel->scene_tree->node, server->cursor->x - server->grab_x,
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

    struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geo_box->x,
                                new_top - geo_box->y);

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
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

    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct sh_server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
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
    }
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

static void output_request_state(struct wl_listener *listener, void *data) {
    struct sh_output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;
    if (wlr_output_commit_state(output->wlr_output, event->state)) {
        update_backgrounds(output->server);
        arrange_layers(output->server);
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
    if (output->server->running && wl_list_empty(&output->server->outputs))
        wl_display_terminate(output->server->wl_display);
    free(output);
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
    struct wlr_box *geometry = &toplevel->xdg_toplevel->base->geometry;
    if (!toplevel->arranged) {
        toplevel->restore_box =
            (struct wlr_box){toplevel->scene_tree->node.x, toplevel->scene_tree->node.y,
                             geometry->width, geometry->height};
    }
    toplevel->arranged = true;
    toplevel->arrangement = action;
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel->foreign, action == SH_MAXIMIZE);
    wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, action == SH_MAXIMIZE);
    wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, action == SH_MAXIMIZE
                                                           ? 0
                                                           : WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
                                                                 WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
    wlr_scene_node_set_position(&toplevel->scene_tree->node, target.x, target.y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, target.width, target.height);
}

static void restore_toplevel(struct sh_toplevel *toplevel) {
    if (!toplevel->arranged)
        return;
    toplevel->arranged = false;
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel->foreign, false);
    wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
    wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, 0);
    wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->restore_box.x,
                                toplevel->restore_box.y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->restore_box.width,
                              toplevel->restore_box.height);
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
        if (toplevel_output(toplevel) == output && !toplevel->minimized)
            ++count;
    }
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (toplevel_output(toplevel) != output || toplevel->minimized)
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
        if (toplevel->arranged && !toplevel->minimized && toplevel_output(toplevel) == output &&
            toplevel->arrangement == SH_TILE)
            ++count;
    }
    wl_list_for_each(toplevel, &server->toplevels, link) {
        if (!toplevel->arranged || toplevel->minimized || toplevel_output(toplevel) != output)
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
    wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
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
    if (toplevel->xdg_toplevel->base->surface->mapped)
        minimize_toplevel(toplevel);
}
static void toplevel_title_changed(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, title_changed);
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_title(
            toplevel->foreign,
            toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "Untitled");
}
static void toplevel_app_id_changed(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, app_id_changed);
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_app_id(
            toplevel->foreign,
            toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "");
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

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data);

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, map);

    int offset = 40 + 32 * (wl_list_length(&toplevel->server->toplevels) % 8);
    int x = offset, y = offset;
    struct wlr_output *output = toplevel_output(toplevel);
    if (output) {
        struct wlr_box area;
        usable_area(toplevel->server, output, &area);
        // Keep newly opened applications reachable inside a small nested output.
        int margin = area.width < 80 || area.height < 80 ? 0 : 40;
        int width = toplevel->xdg_toplevel->base->geometry.width;
        int height = toplevel->xdg_toplevel->base->geometry.height;
        bool resize = false;
        if (width > area.width - 2 * margin) {
            width = area.width - 2 * margin;
            resize = true;
        }
        if (height > area.height - 2 * margin) {
            height = area.height - 2 * margin;
            resize = true;
        }
        if (resize && width > 0 && height > 0)
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, width, height);
        x = area.x + (offset + width <= area.width ? offset : margin);
        y = area.y + (offset + height <= area.height ? offset : margin);
    }
    wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);

    publish_toplevel(toplevel);
    focus_toplevel(toplevel);
    if (toplevel->xdg_toplevel->requested.maximized)
        xdg_toplevel_request_maximize(&toplevel->request_maximize, NULL);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

    if (toplevel == toplevel->server->grabbed_toplevel) {
        reset_cursor_mode(toplevel->server);
    }

    bool was_focused = toplevel->server->focused_toplevel == toplevel;
    if (was_focused)
        deactivate_toplevel(toplevel->server);
    unpublish_toplevel(toplevel);
    wl_list_remove(&toplevel->link);
    if (was_focused)
        focus_previous(toplevel->server);
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

    toplevel->arranged = false;
    if (toplevel->foreign)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel->foreign, false);
    wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
    wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, 0);
    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == SH_CURSOR_MOVE) {
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

        double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
                          ((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
        double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
                          ((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = *geo_box;
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
    if (toplevel->xdg_toplevel->requested.maximized) {
        struct wlr_output *output = toplevel_output(toplevel);
        if (output) {
            struct wlr_box box;
            usable_area(toplevel->server, output, &box);
            place_toplevel(toplevel, SH_MAXIMIZE,
                           (struct sh_rect){box.x, box.y, box.width, box.height});
        }
    } else
        restore_toplevel(toplevel);
    wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct sh_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
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
    while (waitpid(-1, NULL, WNOHANG) > 0) {
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
    server.layer_trees[3] = wlr_scene_tree_create(&server.scene->tree);
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
    unsetenv("DISPLAY");
    server.running = true;
    callbacks->startup(callbacks->userdata);

    wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.wl_display);
    server.running = false;
    wl_event_source_remove(sigint);
    wl_event_source_remove(sigterm);
    wl_event_source_remove(sighup);
    wl_event_source_remove(sigchld);

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

    wlr_backend_destroy(server.backend);
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wl_display_destroy(server.wl_display);
    return 0;
}
