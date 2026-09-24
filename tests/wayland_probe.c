#define _GNU_SOURCE
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

/* A real xdg-shell client: map pixels, wait for a frame, maximize, restore. */
struct buffer {
    struct wl_buffer *object;
    void *pixels;
    size_t size;
    struct buffer *next;
};
struct probe {
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_layer_surface_v1 *panel;
    struct wl_surface *panel_surface;
    struct zwlr_foreign_toplevel_manager_v1 *manager;
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    struct wl_seat *seat;
    struct wl_output *output;
    int output_height;
    bool panel_ready, handle_minimized, handle_active, handle_closed, title_seen;
    struct wl_shm *shm;
    struct xdg_wm_base *shell;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_callback *frame;
    struct buffer *buffers;
    int width, height, stage;
    bool maximized, done, external_control;
};
static void die(const char *message) {
    fprintf(stderr, "wayland probe: %s\n", message);
    exit(1);
}
static void ping(void *data, struct xdg_wm_base *shell, uint32_t serial) {
    xdg_wm_base_pong(shell, serial);
}
static const struct xdg_wm_base_listener shell_listener = {.ping = ping};
static void handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                         const char *title) {
    struct probe *probe = data;
    probe->title_seen = !strcmp(title, "shaoDe protocol probe");
}
static void handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                          const char *app_id) {}
static void handle_output(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                          struct wl_output *output) {}
static void handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                         struct wl_array *states) {
    struct probe *probe = data;
    probe->handle_minimized = probe->handle_active = false;
    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)
            probe->handle_minimized = true;
        if (*state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
            probe->handle_active = true;
    }
}
static void handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    struct probe *probe = data;
    if (probe->stage == 3 && probe->handle_minimized) {
        probe->stage = 4;
        zwlr_foreign_toplevel_handle_v1_unset_minimized(handle);
        zwlr_foreign_toplevel_handle_v1_activate(handle, probe->seat);
    } else if (probe->stage == 4 && probe->handle_active && !probe->handle_minimized) {
        probe->stage = 5;
        zwlr_foreign_toplevel_handle_v1_close(handle);
    }
}
static void handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    struct probe *probe = data;
    probe->handle_closed = true;
    probe->handle = NULL;
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
}
static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = handle_title,
    .app_id = handle_app_id,
    .output_enter = handle_output,
    .output_leave = handle_output,
    .state = handle_state,
    .done = handle_done,
    .closed = handle_closed};
static void manager_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager,
                             struct zwlr_foreign_toplevel_handle_v1 *handle) {
    struct probe *probe = data;
    probe->handle = handle;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, probe);
}
static void manager_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager) {}
static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = manager_toplevel, .finished = manager_finished};
static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t pw,
                            int32_t ph, int32_t subpixel, const char *make, const char *model,
                            int32_t transform) {}
static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t w, int32_t h,
                        int32_t refresh) {
    if (flags & WL_OUTPUT_MODE_CURRENT)
        ((struct probe *)data)->output_height = h;
}
static void output_done(void *data, struct wl_output *output) {}
static void output_scale(void *data, struct wl_output *output, int32_t scale) {}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode, .done = output_done, .scale = output_scale};
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version) {
    struct probe *probe = data;
    if (!strcmp(interface, "wl_compositor"))
        probe->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_shm"))
        probe->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, "zwlr_layer_shell_v1"))
        probe->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 4);
    else if (!strcmp(interface, "zwlr_foreign_toplevel_manager_v1")) {
        probe->manager =
            wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, 2);
        zwlr_foreign_toplevel_manager_v1_add_listener(probe->manager, &manager_listener, probe);
    } else if (!strcmp(interface, "wl_seat"))
        probe->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    else if (!strcmp(interface, "wl_output") && !probe->output) {
        probe->output = wl_registry_bind(registry, name, &wl_output_interface, 2);
        wl_output_add_listener(probe->output, &output_listener, probe);
    } else if (!strcmp(interface, "xdg_wm_base")) {
        probe->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(probe->shell, &shell_listener, probe);
    }
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    struct probe *probe = data;
    wl_callback_destroy(callback);
    probe->frame = NULL;
    if (probe->external_control)
        return;
    if (probe->stage == 0) {
        puts("mapped and received frame");
        probe->stage = 1;
        xdg_toplevel_set_maximized(probe->toplevel);
        wl_surface_commit(probe->surface);
    } else if (probe->stage == 1 && probe->maximized) {
        if (probe->width <= 320 || probe->height <= 240)
            die("maximize did not resize window");
        if (probe->height != probe->output_height - 48)
            die("maximize covered the reserved panel area");
        puts("maximize respected panel reservation and rendered");
        probe->stage = 2;
        xdg_toplevel_unset_maximized(probe->toplevel);
        wl_surface_commit(probe->surface);
    } else if (probe->stage == 2 && !probe->maximized) {
        if (probe->width != 320 || probe->height != 240)
            die("floating size was not restored");
        puts("floating size restored and rendered");
        if (!probe->handle || !probe->title_seen)
            die("window was not published to taskbar clients");
        probe->stage = 3;
        zwlr_foreign_toplevel_handle_v1_set_minimized(probe->handle);
    }
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};
static struct wl_buffer *make_buffer(struct probe *probe, int width, int height) {
    struct buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        die("out of memory");
    buffer->size = (size_t)width * height * 4;
    int fd = memfd_create("shaode-test-buffer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, buffer->size) != 0)
        die("cannot allocate shm buffer");
    buffer->pixels = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer->pixels == MAP_FAILED)
        die("cannot map shm buffer");
    uint32_t *pixels = buffer->pixels;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[y * width + x] = y < 36 ? 0xff23314a : 0xff417bc4;
    struct wl_shm_pool *pool = wl_shm_create_pool(probe->shm, fd, (int)buffer->size);
    buffer->object =
        wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    buffer->next = probe->buffers;
    probe->buffers = buffer;
    return buffer->object;
}
static void panel_configure(void *data, struct zwlr_layer_surface_v1 *panel, uint32_t serial,
                            uint32_t width, uint32_t height) {
    struct probe *probe = data;
    zwlr_layer_surface_v1_ack_configure(panel, serial);
    if (height != 48 || width < 1 || width > 8192)
        die("panel configure geometry invalid");
    struct wl_buffer *buffer = make_buffer(probe, width, height);
    wl_surface_attach(probe->panel_surface, buffer, 0, 0);
    wl_surface_damage_buffer(probe->panel_surface, 0, 0, width, height);
    wl_surface_commit(probe->panel_surface);
    probe->panel_ready = true;
}
static void panel_closed(void *data, struct zwlr_layer_surface_v1 *panel) {
    die("panel closed unexpectedly");
}
static const struct zwlr_layer_surface_v1_listener panel_listener = {.configure = panel_configure,
                                                                     .closed = panel_closed};
static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    struct probe *probe = data;
    xdg_surface_ack_configure(surface, serial);
    if (probe->width < 1 || probe->height < 1 || probe->width > 8192 || probe->height > 8192)
        die("unexpected configure dimensions");
    struct wl_buffer *buffer = make_buffer(probe, probe->width, probe->height);
    wl_surface_attach(probe->surface, buffer, 0, 0);
    wl_surface_damage_buffer(probe->surface, 0, 0, probe->width, probe->height);
    if (!probe->frame) {
        probe->frame = wl_surface_frame(probe->surface);
        wl_callback_add_listener(probe->frame, &frame_listener, probe);
    }
    wl_surface_commit(probe->surface);
}
static const struct xdg_surface_listener surface_listener = {.configure = surface_configure};
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
                               int32_t height, struct wl_array *states) {
    struct probe *probe = data;
    probe->width = width > 0 ? width : 320;
    probe->height = height > 0 ? height : 240;
    probe->maximized = false;
    uint32_t *state;
    wl_array_for_each(state, states) if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED) probe->maximized =
        true;
}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    struct probe *probe = data;
    if (!probe->external_control && probe->stage != 5)
        die("unexpected close request");
    puts("taskbar minimize, restore, activate, and close passed");
    probe->done = true;
}
static const struct xdg_toplevel_listener toplevel_listener = {.configure = toplevel_configure,
                                                               .close = toplevel_close};
int main(int argc, char **argv) {
    struct probe probe = {.width = 320, .height = 240};
    if (argc == 2 && !strcmp(argv[1], "--external-control"))
        probe.external_control = true;
    else if (argc != 1)
        die("usage: wayland_probe [--external-control]");
    struct wl_display *display = wl_display_connect(NULL);
    if (!display)
        die("cannot connect to compositor");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &probe);
    if (wl_display_roundtrip(display) < 0)
        die("registry roundtrip failed");
    if (!probe.compositor || !probe.shm || !probe.shell)
        die("required globals missing");
    if (!probe.layer_shell || !probe.manager || !probe.seat || !probe.output)
        die("desktop protocols missing");
    probe.panel_surface = wl_compositor_create_surface(probe.compositor);
    probe.panel =
        zwlr_layer_shell_v1_get_layer_surface(probe.layer_shell, probe.panel_surface, probe.output,
                                              ZWLR_LAYER_SHELL_V1_LAYER_TOP, "shaode-test-panel");
    zwlr_layer_surface_v1_add_listener(probe.panel, &panel_listener, &probe);
    zwlr_layer_surface_v1_set_size(probe.panel, 0, 48);
    zwlr_layer_surface_v1_set_anchor(probe.panel, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                      ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(probe.panel, 48);
    wl_surface_commit(probe.panel_surface);
    while (!probe.panel_ready)
        if (wl_display_dispatch(display) < 0)
            die("panel setup failed");
    if (wl_display_roundtrip(display) < 0)
        die("panel mapping failed");
    probe.surface = wl_compositor_create_surface(probe.compositor);
    probe.xdg_surface = xdg_wm_base_get_xdg_surface(probe.shell, probe.surface);
    xdg_surface_add_listener(probe.xdg_surface, &surface_listener, &probe);
    probe.toplevel = xdg_surface_get_toplevel(probe.xdg_surface);
    xdg_toplevel_add_listener(probe.toplevel, &toplevel_listener, &probe);
    xdg_toplevel_set_title(probe.toplevel, "shaoDe protocol probe");
    xdg_toplevel_set_app_id(probe.toplevel, "shaode-probe");
    wl_surface_commit(probe.surface);
    while (!probe.done)
        if (wl_display_dispatch(display) < 0)
            die("dispatch failed");
    wl_surface_attach(probe.surface, NULL, 0, 0);
    wl_surface_commit(probe.surface);
    if (wl_display_roundtrip(display) < 0)
        die("unmap failed");
    xdg_toplevel_destroy(probe.toplevel);
    xdg_surface_destroy(probe.xdg_surface);
    wl_surface_destroy(probe.surface);
    if (wl_display_roundtrip(display) < 0)
        die("destroy failed");
    for (struct buffer *buffer = probe.buffers, *next; buffer; buffer = next) {
        next = buffer->next;
        wl_buffer_destroy(buffer->object);
        munmap(buffer->pixels, buffer->size);
        free(buffer);
    }
    if (!probe.handle_closed)
        die("taskbar window handle survived unmapping");
    zwlr_layer_surface_v1_destroy(probe.panel);
    wl_surface_destroy(probe.panel_surface);
    zwlr_layer_shell_v1_destroy(probe.layer_shell);
    zwlr_foreign_toplevel_manager_v1_destroy(probe.manager);
    wl_seat_destroy(probe.seat);
    wl_output_destroy(probe.output);
    xdg_wm_base_destroy(probe.shell);
    wl_shm_destroy(probe.shm);
    wl_compositor_destroy(probe.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    puts("unmap and destroy passed");
    return 0;
}
