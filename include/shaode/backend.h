#pragma once

#include <stdbool.h>
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
    SH_RELOAD
};

/* Modifier bits intentionally match wlroots, without importing its headers. */
enum sh_modifier { SH_SHIFT = 1, SH_CTRL = 4, SH_ALT = 8, SH_LOGO = 64 };

struct sh_settings {
    float background[4];
    uint32_t mouse_modifier;
    int repeat_rate;
    int repeat_delay;
    int gap;
    char keyboard_layout[128];
    char keyboard_options[128];
};

struct sh_callbacks {
    void *userdata;
    const struct sh_settings *(*settings)(void *);
    enum sh_action (*key)(void *, uint32_t modifiers, uint32_t keysym);
    bool (*reload)(void *);
    void (*startup)(void *);
    void (*child_exited)(void *, int pid);
};

enum sh_backend_mode { SH_BACKEND_NESTED, SH_BACKEND_HEADLESS, SH_BACKEND_SESSION };
int sh_run(const struct sh_callbacks *callbacks, enum sh_backend_mode mode);

struct sh_rect {
    int x, y, width, height;
};
bool sh_placement(enum sh_action action, struct sh_rect area, int gap, int index, int count,
                  struct sh_rect *result);

#ifdef __cplusplus
}
#endif
