/* Session-lock client: locking, rejection, focus isolation, unlock, and abandonment. */
#define _GNU_SOURCE
#include "ext-session-lock-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

struct probe {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_output *output;
    struct xdg_wm_base *shell;
    struct ext_session_lock_manager_v1 *manager;
    bool idle_notifier, idle_inhibit;
    int output_width, output_height;
    // Lock state.
    bool locked, finished, rejected_finished, lock_configured;
    int lock_width, lock_height;
    // Window state.
    bool window_configured, window_activated, ever_activated;
};

static void die(const char *message) {
    fprintf(stderr, "lock probe: %s\n", message);
    exit(1);
}

static double now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec / 1e9;
}

/* Dispatch until the condition holds or the timeout expires; returns the condition. */
#define DISPATCH_UNTIL(probe, condition, seconds)                                                  \
    ({                                                                                             \
        double deadline = now() + (seconds);                                                       \
        while (!(condition) && now() < deadline) {                                                 \
            if (wl_display_flush((probe)->display) < 0)                                            \
                die("flush failed");                                                               \
            struct pollfd fd = {wl_display_get_fd((probe)->display), POLLIN, 0};                   \
            if (wl_display_prepare_read((probe)->display) == 0) {                                  \
                if (poll(&fd, 1, 20) > 0)                                                          \
                    wl_display_read_events((probe)->display);                                      \
                else                                                                               \
                    wl_display_cancel_read((probe)->display);                                      \
            }                                                                                      \
            if (wl_display_dispatch_pending((probe)->display) < 0)                                 \
                die("dispatch failed");                                                            \
        }                                                                                          \
        (condition);                                                                               \
    })

static struct wl_buffer *make_buffer(struct probe *probe, int width, int height, uint32_t color) {
    size_t size = (size_t)width * height * 4;
    int fd = memfd_create("shaode-lock-buffer", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        die("cannot allocate shm buffer");
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED)
        die("cannot map shm buffer");
    for (size_t i = 0; i < size / 4; ++i)
        pixels[i] = color;
    munmap(pixels, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(probe->shm, fd, (int)size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y, int32_t pw,
                            int32_t ph, int32_t subpixel, const char *make, const char *model,
                            int32_t transform) {}
static void output_mode(void *data, struct wl_output *output, uint32_t flags, int32_t w, int32_t h,
                        int32_t refresh) {
    struct probe *probe = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        probe->output_width = w;
        probe->output_height = h;
    }
}
static void output_done(void *data, struct wl_output *output) {}
static void output_scale(void *data, struct wl_output *output, int32_t scale) {}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode, .done = output_done, .scale = output_scale};

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
    else if (!strcmp(interface, "wl_output") && !probe->output) {
        probe->output = wl_registry_bind(registry, name, &wl_output_interface, 2);
        wl_output_add_listener(probe->output, &output_listener, probe);
    } else if (!strcmp(interface, "xdg_wm_base")) {
        probe->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(probe->shell, &shell_listener, probe);
    } else if (!strcmp(interface, "ext_session_lock_manager_v1"))
        probe->manager =
            wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1);
    else if (!strcmp(interface, "ext_idle_notifier_v1"))
        probe->idle_notifier = true;
    else if (!strcmp(interface, "zwp_idle_inhibit_manager_v1"))
        probe->idle_inhibit = true;
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};

static void lock_locked(void *data, struct ext_session_lock_v1 *lock) {
    ((struct probe *)data)->locked = true;
}
static void lock_finished(void *data, struct ext_session_lock_v1 *lock) {
    ((struct probe *)data)->finished = true;
}
static const struct ext_session_lock_v1_listener lock_listener = {.locked = lock_locked,
                                                                  .finished = lock_finished};
static void rejected_locked(void *data, struct ext_session_lock_v1 *lock) {
    die("a second lock was granted while the session was locked");
}
static void rejected_finished(void *data, struct ext_session_lock_v1 *lock) {
    ((struct probe *)data)->rejected_finished = true;
}
static const struct ext_session_lock_v1_listener rejected_listener = {
    .locked = rejected_locked, .finished = rejected_finished};

static void lock_surface_configure(void *data, struct ext_session_lock_surface_v1 *surface,
                                   uint32_t serial, uint32_t width, uint32_t height) {
    struct probe *probe = data;
    ext_session_lock_surface_v1_ack_configure(surface, serial);
    probe->lock_width = width;
    probe->lock_height = height;
    probe->lock_configured = true;
}
static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {
    .configure = lock_surface_configure};

static void window_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    ((struct probe *)data)->window_configured = true;
}
static const struct xdg_surface_listener window_surface_listener = {.configure =
                                                                        window_surface_configure};
static void window_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
                             int32_t height, struct wl_array *states) {
    struct probe *probe = data;
    probe->window_activated = false;
    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_ACTIVATED)
            probe->window_activated = probe->ever_activated = true;
    }
}
static void window_close(void *data, struct xdg_toplevel *toplevel) {}
static const struct xdg_toplevel_listener window_listener = {.configure = window_configure,
                                                             .close = window_close};

/* Lock the session and show a lock surface; returns once `locked` arrives. */
static struct ext_session_lock_v1 *lock_session(struct probe *probe) {
    probe->locked = probe->finished = probe->lock_configured = false;
    struct ext_session_lock_v1 *lock = ext_session_lock_manager_v1_lock(probe->manager);
    ext_session_lock_v1_add_listener(lock, &lock_listener, probe);
    struct wl_surface *surface = wl_compositor_create_surface(probe->compositor);
    struct ext_session_lock_surface_v1 *lock_surface =
        ext_session_lock_v1_get_lock_surface(lock, surface, probe->output);
    ext_session_lock_surface_v1_add_listener(lock_surface, &lock_surface_listener, probe);
    if (!DISPATCH_UNTIL(probe, probe->lock_configured || probe->finished, 5) || probe->finished)
        die("lock surface was not configured");
    if (probe->lock_width != probe->output_width || probe->lock_height != probe->output_height)
        die("lock surface does not cover the output");
    wl_surface_attach(surface,
                      make_buffer(probe, probe->lock_width, probe->lock_height, 0xff000000), 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, probe->lock_width, probe->lock_height);
    wl_surface_commit(surface);
    if (!DISPATCH_UNTIL(probe, probe->locked || probe->finished, 5) || probe->finished)
        die("compositor did not confirm the lock");
    return lock;
}

/* Map an ordinary window; it must stay inactive while the session is locked. */
static void map_window(struct probe *probe) {
    struct wl_surface *surface = wl_compositor_create_surface(probe->compositor);
    struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(probe->shell, surface);
    xdg_surface_add_listener(xdg_surface, &window_surface_listener, probe);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
    xdg_toplevel_add_listener(toplevel, &window_listener, probe);
    xdg_toplevel_set_app_id(toplevel, "shaode-lock-probe-window");
    wl_surface_commit(surface);
    if (!DISPATCH_UNTIL(probe, probe->window_configured, 5))
        die("window was not configured");
    wl_surface_attach(surface, make_buffer(probe, 200, 150, 0xffff0000), 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, 200, 150);
    wl_surface_commit(surface);
    DISPATCH_UNTIL(probe, probe->ever_activated, 0.5);
    if (probe->ever_activated)
        die("a window was focused while the session was locked");
}

int main(int argc, char **argv) {
    const char *mode = argc == 2 ? argv[1] : "";
    if (strcmp(mode, "cycle") && strcmp(mode, "abandon") && strcmp(mode, "check-locked"))
        die("usage: lock_probe cycle|abandon|check-locked");
    struct probe probe = {0};
    probe.display = wl_display_connect(NULL);
    if (!probe.display)
        die("cannot connect to compositor");
    struct wl_registry *registry = wl_display_get_registry(probe.display);
    wl_registry_add_listener(registry, &registry_listener, &probe);
    if (wl_display_roundtrip(probe.display) < 0 || wl_display_roundtrip(probe.display) < 0)
        die("registry roundtrip failed");
    if (!probe.compositor || !probe.shm || !probe.output || !probe.shell || !probe.manager)
        die("session lock globals missing");
    if (!probe.idle_notifier || !probe.idle_inhibit)
        die("idle notify/inhibit globals missing");

    if (!strcmp(mode, "check-locked")) {
        // An abandoned lock must keep refusing focus to new windows.
        map_window(&probe);
        puts("abandoned lock still refuses focus");
        return 0;
    }

    struct ext_session_lock_v1 *lock = lock_session(&probe);
    puts("session locked with a full-output lock surface");
    if (!strcmp(mode, "abandon")) {
        puts("abandoning the lock");
        return 0; // Disconnect without unlocking, like a crashed locker.
    }

    struct ext_session_lock_v1 *second = ext_session_lock_manager_v1_lock(probe.manager);
    ext_session_lock_v1_add_listener(second, &rejected_listener, &probe);
    if (!DISPATCH_UNTIL(&probe, probe.rejected_finished, 5))
        die("a second lock was not rejected");
    ext_session_lock_v1_destroy(second);
    puts("second locker rejected");

    map_window(&probe);
    puts("windows cannot take focus while locked");

    ext_session_lock_v1_unlock_and_destroy(lock);
    if (!DISPATCH_UNTIL(&probe, probe.window_activated, 5))
        die("focus did not return after unlocking");
    puts("unlock returned focus to the desktop");

    lock = lock_session(&probe);
    ext_session_lock_v1_unlock_and_destroy(lock);
    if (wl_display_roundtrip(probe.display) < 0)
        die("final unlock failed");
    puts("session can be locked again after unlocking");
    return 0;
}
