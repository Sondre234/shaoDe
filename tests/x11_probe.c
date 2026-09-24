/* X11 client exercising XWayland mapping, focus, fullscreen, and close. */
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

struct probe {
    xcb_connection_t *connection;
    xcb_window_t root, window;
    xcb_atom_t protocols, delete_window, state, fullscreen;
    int width, height;
    bool mapped, closed;
};

static void die(const char *message) {
    fprintf(stderr, "x11 probe: %s\n", message);
    exit(1);
}

static xcb_atom_t atom(xcb_connection_t *connection, const char *name) {
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
        connection, xcb_intern_atom(connection, false, strlen(name), name), NULL);
    if (!reply)
        die("cannot intern atom");
    xcb_atom_t result = reply->atom;
    free(reply);
    return result;
}

static double now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec / 1e9;
}

static void handle(struct probe *probe, xcb_generic_event_t *event) {
    switch (event->response_type & ~0x80) {
    case XCB_MAP_NOTIFY:
        probe->mapped = true;
        break;
    case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t *configure = (xcb_configure_notify_event_t *)event;
        if (configure->window == probe->window) {
            probe->width = configure->width;
            probe->height = configure->height;
        }
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *message = (xcb_client_message_event_t *)event;
        if (message->type == probe->protocols && message->data.data32[0] == probe->delete_window)
            probe->closed = true;
        break;
    }
    }
}

/* Dispatch events until the condition holds, failing after a few seconds. */
#define WAIT_FOR(probe, condition, message)                                                        \
    do {                                                                                           \
        double deadline = now() + 5;                                                               \
        while (!(condition)) {                                                                     \
            xcb_generic_event_t *event;                                                            \
            while (!(condition) && (event = xcb_poll_for_event((probe)->connection))) {            \
                handle(probe, event);                                                              \
                free(event);                                                                       \
            }                                                                                      \
            if (condition)                                                                         \
                break;                                                                             \
            if (xcb_connection_has_error((probe)->connection))                                     \
                die("X connection failed while waiting: " message);                                \
            if (now() > deadline)                                                                  \
                die("timed out: " message);                                                        \
            struct pollfd fd = {xcb_get_file_descriptor((probe)->connection), POLLIN, 0};          \
            poll(&fd, 1, 50);                                                                      \
        }                                                                                          \
    } while (0)

static void request_fullscreen(struct probe *probe, bool enable) {
    xcb_client_message_event_t message = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format = 32,
        .window = probe->window,
        .type = probe->state,
        .data.data32 = {enable ? 1 : 0, probe->fullscreen, 0, 1, 0}};
    xcb_send_event(probe->connection, false, probe->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   (const char *)&message);
    xcb_flush(probe->connection);
}

int main(int argc, char **argv) {
    const char *command = argc == 2 ? argv[1] : "full";
    struct probe probe = {0};
    probe.connection = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(probe.connection))
        die("cannot connect to X server");
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(probe.connection)).data;
    probe.root = screen->root;
    probe.protocols = atom(probe.connection, "WM_PROTOCOLS");
    probe.delete_window = atom(probe.connection, "WM_DELETE_WINDOW");
    probe.state = atom(probe.connection, "_NET_WM_STATE");
    probe.fullscreen = atom(probe.connection, "_NET_WM_STATE_FULLSCREEN");

    probe.window = xcb_generate_id(probe.connection);
    uint32_t values[] = {screen->white_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(probe.connection, XCB_COPY_FROM_PARENT, probe.window, probe.root, 0, 0, 300,
                      200, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    const char title[] = "shaoDe X11 probe", class[] = "shaode-x11-probe\0shaode-x11-probe";
    xcb_change_property(probe.connection, XCB_PROP_MODE_REPLACE, probe.window, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, strlen(title), title);
    xcb_change_property(probe.connection, XCB_PROP_MODE_REPLACE, probe.window, XCB_ATOM_WM_CLASS,
                        XCB_ATOM_STRING, 8, sizeof(class), class);
    xcb_change_property(probe.connection, XCB_PROP_MODE_REPLACE, probe.window, probe.protocols,
                        XCB_ATOM_ATOM, 32, 1, &probe.delete_window);
    xcb_map_window(probe.connection, probe.window);
    xcb_flush(probe.connection);
    WAIT_FOR(&probe, probe.mapped, "window mapping");

    // The compositor gives newly mapped windows keyboard focus.
    double deadline = now() + 5;
    for (;;) {
        xcb_get_input_focus_reply_t *focus = xcb_get_input_focus_reply(
            probe.connection, xcb_get_input_focus(probe.connection), NULL);
        bool focused = focus && focus->focus == probe.window;
        free(focus);
        if (focused)
            break;
        if (now() > deadline)
            die("mapped window did not receive input focus");
        struct timespec pause = {0, 20 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    puts("X11 window mapped and focused");

    if (!strcmp(command, "wait-close")) {
        // The harness closes this window through the compositor.
        puts("waiting for close");
        fflush(stdout);
        WAIT_FOR(&probe, probe.closed, "compositor close request");
        puts("X11 close request received");
        xcb_disconnect(probe.connection);
        return 0;
    }

    xcb_get_geometry_reply_t *root = xcb_get_geometry_reply(
        probe.connection, xcb_get_geometry(probe.connection, probe.root), NULL);
    if (!root)
        die("cannot query root geometry");
    int screen_width = root->width, screen_height = root->height;
    free(root);

    request_fullscreen(&probe, true);
    WAIT_FOR(&probe, probe.width == screen_width && probe.height == screen_height,
             "fullscreen configure");
    puts("X11 fullscreen covered the screen");
    request_fullscreen(&probe, false);
    WAIT_FOR(&probe, probe.width == 300 && probe.height == 200, "fullscreen exit");
    puts("X11 fullscreen exit restored size");

    xcb_destroy_window(probe.connection, probe.window);
    xcb_flush(probe.connection);
    xcb_disconnect(probe.connection);
    return 0;
}
