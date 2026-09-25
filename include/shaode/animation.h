// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/* Short window animations on the plain wlroots scene graph: a window fades in and grows
 * slightly when it opens, a snapshot of its last frame fades out and shrinks when it closes,
 * and tiles glide to their new place when the layout changes.
 *
 * Each animation owns one scene tree and changes only what is below it: the tree's own
 * position (the glide), and the position, destination size, and opacity (or color, for
 * rectangles) of every buffer and rectangle inside it. The scene has no transform for a whole
 * subtree, so scaling moves and resizes each buffer about a common center. Clients and the
 * rest of the compositor keep writing to those nodes while an animation runs; a value that
 * differs from what the animation last set is taken as the node's new resting value. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wlr_scene_node;
struct wlr_scene_tree;
struct sh_animator;
struct sh_anim_record;

/* One window's running animation, embedded in the window and zeroed with it. */
struct sh_anim {
    struct sh_animator *animator; // set while it runs
    struct wlr_scene_tree *tree;
    struct wl_list link;
    bool owned; // a closing snapshot: the tree and this struct are freed when it ends
    bool fx, glide;
    int64_t fx_start, glide_start; // milliseconds, CLOCK_MONOTONIC
    float scale_from, scale_to, alpha_from, alpha_to;
    double cx, cy;        // scale center, in tree coordinates
    int glide_x, glide_y; // the tree's offset when the glide started; it ends at 0, 0
    struct sh_anim_record *records;
    size_t count, capacity;
};

struct sh_animator *sh_animator_create(struct wl_event_loop *loop);
/* Ends every animation; closing snapshots are destroyed. Call before the scene goes. */
void sh_animator_destroy(struct sh_animator *animator);
/* Disabling finishes what is running. */
void sh_animator_configure(struct sh_animator *animator, bool enabled, int duration_ms);
/* Advances every animation to the current time; call right before rendering a frame. */
void sh_animator_tick(struct sh_animator *animator);
size_t sh_animator_running(const struct sh_animator *animator);

/* Fades `tree` in while it grows to full size about (cx, cy). */
void sh_anim_open(struct sh_animator *animator, struct sh_anim *anim, struct wlr_scene_tree *tree,
                  double cx, double cy);
/* Offsets `tree` by (dx, dy) from where it now rests and glides it back. */
void sh_anim_glide(struct sh_animator *animator, struct sh_anim *anim, struct wlr_scene_tree *tree,
                   int dx, int dy);
/* Jumps to the end: every node gets its resting values and the tree its resting position. */
void sh_anim_finish(struct sh_anim *anim);
/* Copies the visible buffers and rectangles under `content` into a new tree just above
 * `window`, which fades out and shrinks about (cx, cy) in `content` coordinates, then goes. */
void sh_anim_close(struct sh_animator *animator, struct wlr_scene_node *window,
                   struct wlr_scene_tree *content, double cx, double cy);
