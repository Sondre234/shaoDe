#include "shaode/decoration.h"
#include <drm_fourcc.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>

enum { DOT_RADIUS = 6, SAMPLES = 4 };
static const double dot_x[3] = {11, 32, 53}; // close, minimize, fullscreen
static const double dot_y = SH_DECO_HEIGHT / 2.0;

struct pixel_buffer {
    struct wlr_buffer base;
    uint32_t *data;
    size_t stride;
};

static void pixel_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct pixel_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
    wlr_buffer_finish(wlr_buffer);
    free(buffer->data);
    free(buffer);
}

static bool pixel_buffer_begin(struct wlr_buffer *wlr_buffer, uint32_t flags, void **data,
                               uint32_t *format, size_t *stride) {
    struct pixel_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
        return false;
    *data = buffer->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = buffer->stride;
    return true;
}

static void pixel_buffer_end(struct wlr_buffer *wlr_buffer) {}

static const struct wlr_buffer_impl pixel_buffer_impl = {
    .destroy = pixel_buffer_destroy,
    .begin_data_ptr_access = pixel_buffer_begin,
    .end_data_ptr_access = pixel_buffer_end,
};

struct rgba {
    double r, g, b, a; // premultiplied
};

static struct rgba color(uint32_t rgb, double alpha) {
    return (struct rgba){((rgb >> 16) & 0xff) / 255.0 * alpha, ((rgb >> 8) & 0xff) / 255.0 * alpha,
                         (rgb & 0xff) / 255.0 * alpha, alpha};
}

static void over(struct rgba *dst, struct rgba src) {
    dst->r = src.r + dst->r * (1 - src.a);
    dst->g = src.g + dst->g * (1 - src.a);
    dst->b = src.b + dst->b * (1 - src.a);
    dst->a = src.a + dst->a * (1 - src.a);
}

static double segment_distance(double x, double y, double x0, double y0, double x1, double y1) {
    double dx = x1 - x0, dy = y1 - y0;
    double t = ((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypot(x - (x0 + t * dx), y - (y0 + t * dy));
}

/* Whether the glyph of dot `dot` covers dot-local point (x, y). */
static bool glyph_covers(int dot, double x, double y) {
    const double half = 0.65, reach = 2.7;
    switch (dot) {
    case 0: // ×
        return segment_distance(x, y, -reach, -reach, reach, reach) < half ||
               segment_distance(x, y, -reach, reach, reach, -reach) < half;
    case 1: // −
        return fabs(y) < half && fabs(x) < reach + 0.5;
    default: // two corner triangles, as on macOS
        return (x >= -2.6 && y >= -2.6 && x + y <= -1.4) || (x <= 2.6 && y <= 2.6 && x + y >= 1.4);
    }
}

static struct rgba sample(double x, double y, bool hovered) {
    static const uint32_t fills[3] = {0xff5f57, 0xfebc2e, 0x28c840};
    static const uint32_t rims[3] = {0xe0443e, 0xdea123, 0x1aab29};
    struct rgba pixel = {0};
    // Rounded pill: a rectangle with semicircular ends.
    double radius = SH_DECO_HEIGHT / 2.0;
    double cx = x < radius ? radius : x > SH_DECO_WIDTH - radius ? SH_DECO_WIDTH - radius : x;
    if (hypot(x - cx, y - radius) <= radius)
        over(&pixel, color(0x16181d, 0.42));
    for (int dot = 0; dot < 3; ++dot) {
        double dx = x - dot_x[dot], dy = y - dot_y;
        double distance = hypot(dx, dy);
        if (distance > DOT_RADIUS)
            continue;
        over(&pixel, color(distance > DOT_RADIUS - 0.6 ? rims[dot] : fills[dot], 1));
        if (hovered && glyph_covers(dot, dx, dy))
            over(&pixel, color(0x000000, 0.55));
    }
    return pixel;
}

struct wlr_buffer *sh_decoration_render(int scale, bool hovered) {
    if (scale < 1)
        scale = 1;
    int width = SH_DECO_WIDTH * scale, height = SH_DECO_HEIGHT * scale;
    struct pixel_buffer *buffer = calloc(1, sizeof(*buffer));
    uint32_t *data = calloc((size_t)width * height, sizeof(*data));
    if (!buffer || !data) {
        free(buffer);
        free(data);
        return NULL;
    }
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            struct rgba sum = {0};
            for (int sy = 0; sy < SAMPLES; ++sy) {
                for (int sx = 0; sx < SAMPLES; ++sx) {
                    struct rgba s = sample((px + (sx + 0.5) / SAMPLES) / scale,
                                           (py + (sy + 0.5) / SAMPLES) / scale, hovered);
                    sum.r += s.r, sum.g += s.g, sum.b += s.b, sum.a += s.a;
                }
            }
            double n = SAMPLES * SAMPLES;
            data[py * width + px] =
                (uint32_t)lround(sum.a / n * 255) << 24 | (uint32_t)lround(sum.r / n * 255) << 16 |
                (uint32_t)lround(sum.g / n * 255) << 8 | (uint32_t)lround(sum.b / n * 255);
        }
    }
    buffer->data = data;
    buffer->stride = (size_t)width * sizeof(*data);
    wlr_buffer_init(&buffer->base, &pixel_buffer_impl, width, height);
    return &buffer->base;
}

enum sh_deco_part sh_decoration_part_at(double x, double y) {
    if (x < 0 || y < 0 || x >= SH_DECO_WIDTH || y >= SH_DECO_HEIGHT)
        return SH_DECO_NONE;
    // Hit targets are a little larger than the dots, as they are small.
    for (int dot = 0; dot < 3; ++dot) {
        if (hypot(x - dot_x[dot], y - dot_y) <= DOT_RADIUS + 2)
            return SH_DECO_CLOSE + dot;
    }
    return SH_DECO_PILL;
}
