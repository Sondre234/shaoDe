#define _GNU_SOURCE
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
    struct wl_shm *shm;
    struct xdg_wm_base *shell;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_callback *frame;
    struct buffer *buffers;
    int width, height, stage;
    bool maximized, done;
};
static void die(const char *message) {
    fprintf(stderr, "wayland probe: %s\n", message);
    exit(1);
}
static void ping(void *data, struct xdg_wm_base *shell, uint32_t serial) {
    xdg_wm_base_pong(shell, serial);
}
static const struct xdg_wm_base_listener shell_listener = {.ping = ping};
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version) {
    struct probe *probe = data;
    if (!strcmp(interface, "wl_compositor"))
        probe->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_shm"))
        probe->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, "xdg_wm_base")) {
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
    if (probe->stage == 0) {
        puts("mapped and received frame");
        probe->stage = 1;
        xdg_toplevel_set_maximized(probe->toplevel);
        wl_surface_commit(probe->surface);
    } else if (probe->stage == 1 && probe->maximized) {
        if (probe->width <= 320 || probe->height <= 240)
            die("maximize did not resize window");
        puts("maximize configured and rendered");
        probe->stage = 2;
        xdg_toplevel_unset_maximized(probe->toplevel);
        wl_surface_commit(probe->surface);
    } else if (probe->stage == 2 && !probe->maximized) {
        if (probe->width != 320 || probe->height != 240)
            die("floating size was not restored");
        puts("floating size restored and rendered");
        probe->done = true;
    }
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};
static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    struct probe *probe = data;
    xdg_surface_ack_configure(surface, serial);
    if (probe->width < 1 || probe->height < 1 || probe->width > 8192 || probe->height > 8192)
        die("unexpected configure dimensions");
    struct buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        die("out of memory");
    buffer->size = (size_t)probe->width * probe->height * 4;
    int fd = memfd_create("shaode-test-buffer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, buffer->size) != 0)
        die("cannot allocate shm buffer");
    buffer->pixels = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer->pixels == MAP_FAILED)
        die("cannot map shm buffer");
    uint32_t *pixels = buffer->pixels;
    for (int y = 0; y < probe->height; ++y)
        for (int x = 0; x < probe->width; ++x)
            pixels[y * probe->width + x] = y < 36 ? 0xff23314a : 0xff417bc4;
    struct wl_shm_pool *pool = wl_shm_create_pool(probe->shm, fd, (int)buffer->size);
    buffer->object = wl_shm_pool_create_buffer(pool, 0, probe->width, probe->height,
                                               probe->width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    buffer->next = probe->buffers;
    probe->buffers = buffer;
    wl_surface_attach(probe->surface, buffer->object, 0, 0);
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
    die("unexpected close request");
}
static const struct xdg_toplevel_listener toplevel_listener = {.configure = toplevel_configure,
                                                               .close = toplevel_close};
int main(void) {
    struct probe probe = {.width = 320, .height = 240};
    struct wl_display *display = wl_display_connect(NULL);
    if (!display)
        die("cannot connect to compositor");
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, &probe);
    if (wl_display_roundtrip(display) < 0)
        die("registry roundtrip failed");
    if (!probe.compositor || !probe.shm || !probe.shell)
        die("required globals missing");
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
    xdg_wm_base_destroy(probe.shell);
    wl_shm_destroy(probe.shm);
    wl_compositor_destroy(probe.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    puts("unmap and destroy passed");
    return 0;
}
