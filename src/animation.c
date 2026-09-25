/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "shaode/animation.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_scene.h>

/* How much smaller a window is when it starts opening or finishes closing. */
static const float SMALL = 0.94F;
/* After this much time past the end, a timer finishes animations no frame has advanced, e.g.
 * on a hidden workspace or a switched-off output. */
enum { SLACK_MS = 50 };

struct sh_animator {
    bool enabled;
    int duration;           // milliseconds
    struct wl_list running; // struct sh_anim
    struct wl_event_source *timer;
};

/* A node's resting values and the values the animation last gave it. */
struct sh_anim_record {
    struct wlr_scene_node *node;
    bool shaped, painted; // the resting geometry, paint are known
    int x, y, width, height;
    int set_x, set_y, set_width, set_height;
    float paint[4], set_paint[4]; // opacity in [0], or a rectangle's color
};

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Ease-out cubic: fast at first, settling gently. */
static double eased(int64_t elapsed, int duration) {
    double t = duration > 0 ? (double)elapsed / duration : 1;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

static struct sh_anim_record *record(struct sh_anim *anim, struct wlr_scene_node *node) {
    for (size_t i = 0; i < anim->count; ++i)
        if (anim->records[i].node == node)
            return &anim->records[i];
    if (anim->count == anim->capacity) {
        size_t capacity = anim->capacity ? 2 * anim->capacity : 8;
        struct sh_anim_record *records = realloc(anim->records, capacity * sizeof(*records));
        if (!records)
            return NULL;
        anim->records = records;
        anim->capacity = capacity;
    }
    struct sh_anim_record *result = &anim->records[anim->count++];
    *result = (struct sh_anim_record){.node = node};
    return result;
}

/* A node's current geometry and paint. Buffers without a destination size are not resized. */
static bool geometry(struct wlr_scene_node *node, int *width, int *height) {
    if (node->type == WLR_SCENE_NODE_RECT) {
        struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
        *width = rect->width, *height = rect->height;
    } else {
        struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
        *width = buffer->dst_width, *height = buffer->dst_height;
    }
    return *width > 0 && *height > 0;
}
static void paint(struct wlr_scene_node *node, float value[4]) {
    if (node->type == WLR_SCENE_NODE_RECT)
        memcpy(value, wlr_scene_rect_from_node(node)->color, 4 * sizeof(float));
    else
        value[0] = wlr_scene_buffer_from_node(node)->opacity, value[1] = value[2] = value[3] = 0;
}
static void set_geometry(struct wlr_scene_node *node, int x, int y, int width, int height) {
    wlr_scene_node_set_position(node, x, y);
    if (node->type == WLR_SCENE_NODE_RECT)
        wlr_scene_rect_set_size(wlr_scene_rect_from_node(node), width, height);
    else
        wlr_scene_buffer_set_dest_size(wlr_scene_buffer_from_node(node), width, height);
}
static void set_paint(struct wlr_scene_node *node, const float value[4]) {
    if (node->type == WLR_SCENE_NODE_RECT)
        wlr_scene_rect_set_color(wlr_scene_rect_from_node(node), value);
    else
        wlr_scene_buffer_set_opacity(wlr_scene_buffer_from_node(node), value[0]);
}

/* Scales the node about the center and multiplies its opacity. `ox, oy` is the position of
 * its parent in the animated tree. */
static void apply_node(struct sh_anim *anim, struct wlr_scene_node *node, int ox, int oy,
                       float scale, float alpha) {
    struct sh_anim_record *r = record(anim, node);
    if (!r)
        return;
    int width, height;
    bool sized = geometry(node, &width, &height);
    if (!r->shaped || node->x != r->set_x || node->y != r->set_y || width != r->set_width ||
        height != r->set_height) {
        r->shaped = true;
        r->x = node->x, r->y = node->y, r->width = width, r->height = height;
    }
    float current[4];
    paint(node, current);
    if (!r->painted || memcmp(current, r->set_paint, sizeof(current))) {
        r->painted = true;
        memcpy(r->paint, current, sizeof(current));
    }
    if (sized) {
        double left = anim->cx + (ox + r->x - anim->cx) * scale;
        double top = anim->cy + (oy + r->y - anim->cy) * scale;
        r->set_x = (int)lround(left) - ox, r->set_y = (int)lround(top) - oy;
        r->set_width = (int)fmax(1, lround(r->width * scale));
        r->set_height = (int)fmax(1, lround(r->height * scale));
        set_geometry(node, r->set_x, r->set_y, r->set_width, r->set_height);
    } else {
        r->set_x = r->x, r->set_y = r->y, r->set_width = width, r->set_height = height;
    }
    // Opacity is a factor; a premultiplied color fades by multiplying every channel.
    for (int i = 0; i < 4; ++i)
        r->set_paint[i] = r->paint[i] * alpha;
    set_paint(node, r->set_paint);
}

static void restore_node(struct sh_anim *anim, struct wlr_scene_node *node) {
    struct sh_anim_record *r = NULL;
    for (size_t i = 0; i < anim->count && !r; ++i)
        if (anim->records[i].node == node)
            r = &anim->records[i];
    if (!r)
        return;
    int width, height;
    bool sized = geometry(node, &width, &height);
    // Leave values that changed under the animation: they are newer than the record.
    if (r->shaped && sized && node->x == r->set_x && node->y == r->set_y && width == r->set_width &&
        height == r->set_height)
        set_geometry(node, r->x, r->y, r->width, r->height);
    float current[4];
    paint(node, current);
    if (r->painted && !memcmp(current, r->set_paint, sizeof(current)))
        set_paint(node, r->paint);
}

/* Visits every buffer and rectangle below `tree`, with its parent's offset from the root. */
static void walk(struct sh_anim *anim, struct wlr_scene_tree *tree, int ox, int oy, bool restore,
                 float scale, float alpha) {
    struct wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link) {
        if (node->type == WLR_SCENE_NODE_TREE)
            walk(anim, wlr_scene_tree_from_node(node), ox + node->x, oy + node->y, restore, scale,
                 alpha);
        else if (restore)
            restore_node(anim, node);
        else
            apply_node(anim, node, ox, oy, scale, alpha);
    }
}

static void stop(struct sh_anim *anim) {
    if (!anim->animator)
        return;
    wl_list_remove(&anim->link);
    anim->animator = NULL;
    anim->fx = anim->glide = false;
    if (anim->owned) {
        wlr_scene_node_destroy(&anim->tree->node);
        free(anim->records);
        free(anim);
        return;
    }
    free(anim->records);
    anim->records = NULL;
    anim->count = anim->capacity = 0;
}

/* Returns whether the animation is still running. */
static bool step(struct sh_anim *anim, int64_t now) {
    int duration = anim->animator->duration;
    if (anim->glide) {
        double e = eased(now - anim->glide_start, duration);
        wlr_scene_node_set_position(&anim->tree->node, (int)lround(anim->glide_x * (1 - e)),
                                    (int)lround(anim->glide_y * (1 - e)));
        anim->glide = e < 1;
    }
    if (anim->fx) {
        double e = eased(now - anim->fx_start, duration);
        if (e < 1)
            walk(anim, anim->tree, 0, 0, false,
                 (float)(anim->scale_from + (anim->scale_to - anim->scale_from) * e),
                 (float)(anim->alpha_from + (anim->alpha_to - anim->alpha_from) * e));
        else if (!anim->owned)
            walk(anim, anim->tree, 0, 0, true, 1, 1);
        anim->fx = e < 1;
    }
    return anim->fx || anim->glide;
}

void sh_anim_finish(struct sh_anim *anim) {
    if (!anim->animator)
        return;
    if (anim->glide)
        wlr_scene_node_set_position(&anim->tree->node, 0, 0);
    if (anim->fx && !anim->owned)
        walk(anim, anim->tree, 0, 0, true, 1, 1);
    stop(anim);
}

void sh_animator_tick(struct sh_animator *animator) {
    int64_t now = now_ms();
    struct sh_anim *anim, *next;
    wl_list_for_each_safe(anim, next, &animator->running, link) {
        if (!step(anim, now))
            stop(anim);
    }
}

static int timer_fired(void *data) {
    struct sh_animator *animator = data;
    sh_animator_tick(animator);
    if (!wl_list_empty(&animator->running))
        wl_event_source_timer_update(animator->timer, SLACK_MS);
    return 0;
}

static void start(struct sh_animator *animator, struct sh_anim *anim, struct wlr_scene_tree *tree) {
    if (anim->animator && anim->tree != tree)
        sh_anim_finish(anim);
    anim->tree = tree;
    if (!anim->animator) {
        anim->animator = animator;
        wl_list_insert(animator->running.prev, &anim->link);
    }
    wl_event_source_timer_update(animator->timer, animator->duration + SLACK_MS);
}

void sh_anim_open(struct sh_animator *animator, struct sh_anim *anim, struct wlr_scene_tree *tree,
                  double cx, double cy) {
    if (!animator->enabled)
        return;
    start(animator, anim, tree);
    anim->fx = true;
    anim->fx_start = now_ms();
    anim->scale_from = SMALL, anim->scale_to = 1;
    anim->alpha_from = 0, anim->alpha_to = 1;
    anim->cx = cx, anim->cy = cy;
    step(anim, anim->fx_start);
}

void sh_anim_glide(struct sh_animator *animator, struct sh_anim *anim, struct wlr_scene_tree *tree,
                   int dx, int dy) {
    if (!animator->enabled || (dx == 0 && dy == 0))
        return;
    start(animator, anim, tree);
    // A glide under way continues from where the window is drawn now.
    anim->glide_x = tree->node.x + dx;
    anim->glide_y = tree->node.y + dy;
    anim->glide = true;
    anim->glide_start = now_ms();
    step(anim, anim->glide_start);
}

/* Copies what is visible below `tree` into `target`, flattened. */
static void copy(struct wlr_scene_tree *target, struct wlr_scene_tree *tree, int ox, int oy) {
    struct wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link) {
        if (!node->enabled)
            continue;
        int x = ox + node->x, y = oy + node->y;
        if (node->type == WLR_SCENE_NODE_TREE) {
            copy(target, wlr_scene_tree_from_node(node), x, y);
        } else if (node->type == WLR_SCENE_NODE_RECT) {
            struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
            struct wlr_scene_rect *clone =
                wlr_scene_rect_create(target, rect->width, rect->height, rect->color);
            if (clone)
                wlr_scene_node_set_position(&clone->node, x, y);
        } else if (node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
            if (!buffer->buffer)
                continue;
            // The new node locks the client's buffer, which keeps its texture.
            struct wlr_scene_buffer *clone = wlr_scene_buffer_create(target, buffer->buffer);
            if (!clone)
                continue;
            int width = buffer->dst_width, height = buffer->dst_height;
            if (width <= 0 || height <= 0) {
                bool turned = buffer->transform & WL_OUTPUT_TRANSFORM_90;
                width = turned ? buffer->buffer->height : buffer->buffer->width;
                height = turned ? buffer->buffer->width : buffer->buffer->height;
            }
            wlr_scene_node_set_position(&clone->node, x, y);
            wlr_scene_buffer_set_dest_size(clone, width, height);
            wlr_scene_buffer_set_source_box(clone, &buffer->src_box);
            wlr_scene_buffer_set_transform(clone, buffer->transform);
            wlr_scene_buffer_set_opacity(clone, buffer->opacity);
            wlr_scene_buffer_set_filter_mode(clone, buffer->filter_mode);
            wlr_scene_buffer_set_opaque_region(clone, &buffer->opaque_region);
            wlr_scene_buffer_set_transfer_function(clone, buffer->transfer_function);
            wlr_scene_buffer_set_primaries(clone, buffer->primaries);
            wlr_scene_buffer_set_color_encoding(clone, buffer->color_encoding);
            wlr_scene_buffer_set_color_range(clone, buffer->color_range);
        }
    }
}

void sh_anim_close(struct sh_animator *animator, struct wlr_scene_node *window,
                   struct wlr_scene_tree *content, double cx, double cy) {
    if (!animator->enabled || !window->enabled || !window->parent)
        return;
    struct sh_anim *anim = calloc(1, sizeof(*anim));
    struct wlr_scene_tree *tree = anim ? wlr_scene_tree_create(window->parent) : NULL;
    if (!tree) {
        free(anim);
        return;
    }
    copy(tree, content, 0, 0);
    if (wl_list_empty(&tree->children)) {
        wlr_scene_node_destroy(&tree->node);
        free(anim);
        return;
    }
    wlr_scene_node_place_above(&tree->node, window);
    wlr_scene_node_set_position(&tree->node, window->x + content->node.x,
                                window->y + content->node.y);
    anim->owned = true;
    start(animator, anim, tree);
    anim->fx = true;
    anim->fx_start = now_ms();
    anim->scale_from = 1, anim->scale_to = SMALL;
    anim->alpha_from = 1, anim->alpha_to = 0;
    anim->cx = cx, anim->cy = cy;
    step(anim, anim->fx_start);
}

struct sh_animator *sh_animator_create(struct wl_event_loop *loop) {
    struct sh_animator *animator = calloc(1, sizeof(*animator));
    if (!animator)
        return NULL;
    wl_list_init(&animator->running);
    animator->timer = wl_event_loop_add_timer(loop, timer_fired, animator);
    if (!animator->timer) {
        free(animator);
        return NULL;
    }
    return animator;
}

void sh_animator_configure(struct sh_animator *animator, bool enabled, int duration_ms) {
    animator->enabled = enabled && duration_ms > 0;
    animator->duration = duration_ms;
    if (!animator->enabled) {
        struct sh_anim *anim, *next;
        wl_list_for_each_safe(anim, next, &animator->running, link) sh_anim_finish(anim);
    }
}

size_t sh_animator_running(const struct sh_animator *animator) {
    return (size_t)wl_list_length(&animator->running);
}

void sh_animator_destroy(struct sh_animator *animator) {
    if (!animator)
        return;
    sh_animator_configure(animator, false, 0);
    wl_event_source_remove(animator->timer);
    free(animator);
}
