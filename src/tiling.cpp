// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaode/backend.h"
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

/* Dwindle tiling, after Hyprland's default layout of the same name:
 * leaves are windows, inner nodes split their box in two at `ratio`. The split direction
 * follows the box shape at every arrangement, so each tree adapts to any output size. */
namespace {
using Key = std::pair<std::string, int>;

struct Node {
    Node *parent = nullptr;
    std::unique_ptr<Node> children[2];
    void *window = nullptr; // set only on leaves
    double ratio = 0.5;     // share of the first child
    bool horizontal = true; // children side by side, from the last arrangement
    sh_rect box{};          // from the last arrangement, without gaps
};

bool contains(const sh_rect &box, double x, double y) {
    return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
}
bool splits_horizontally(const sh_rect &box) { return box.width >= box.height; }

Node *leaf_at(Node *node, double x, double y) {
    if (node->window)
        return contains(node->box, x, y) ? node : nullptr;
    for (auto &child : node->children)
        if (Node *found = leaf_at(child.get(), x, y))
            return found;
    return nullptr;
}
} // namespace

struct sh_tiling {
    std::map<Key, std::unique_ptr<Node>> roots;
    std::unordered_map<const void *, std::pair<Node *, Key>> leaves;

    std::unique_ptr<Node> &slot(Node *node, const Key &key) {
        if (!node->parent)
            return roots[key];
        auto &children = node->parent->children;
        return children[0].get() == node ? children[0] : children[1];
    }

    void layout(Node *node, sh_rect box, const sh_rect &area, int gap, sh_tile_place place,
                void *userdata) {
        node->box = box;
        if (node->window) {
            // Outer edges keep the full gap; neighbours share one gap between them.
            auto inset = [gap](bool outer) { return outer ? gap : gap / 2; };
            auto inset_end = [gap](bool outer) { return outer ? gap : gap - gap / 2; };
            int left = inset(box.x == area.x);
            int top = inset(box.y == area.y);
            int right = inset_end(box.x + box.width == area.x + area.width);
            int bottom = inset_end(box.y + box.height == area.y + area.height);
            sh_rect rect{box.x + left, box.y + top, std::max(1, box.width - left - right),
                         std::max(1, box.height - top - bottom)};
            place(userdata, node->window, rect);
            return;
        }
        node->horizontal = splits_horizontally(box);
        sh_rect first = box, second = box;
        if (node->horizontal) {
            first.width = std::clamp(static_cast<int>(box.width * node->ratio), 1,
                                     std::max(1, box.width - 1));
            second.x += first.width;
            second.width = std::max(1, box.width - first.width);
        } else {
            first.height = std::clamp(static_cast<int>(box.height * node->ratio), 1,
                                      std::max(1, box.height - 1));
            second.y += first.height;
            second.height = std::max(1, box.height - first.height);
        }
        layout(node->children[0].get(), first, area, gap, place, userdata);
        layout(node->children[1].get(), second, area, gap, place, userdata);
    }
};

extern "C" {
sh_tiling *sh_tiling_create(void) { return new (std::nothrow) sh_tiling; }
void sh_tiling_destroy(sh_tiling *tiling) { delete tiling; }

void sh_tiling_insert(sh_tiling *tiling, const char *output, int workspace, void *window,
                      const void *target, bool has_point, double x, double y) {
    if (!tiling || !output || !window || tiling->leaves.contains(window))
        return;
    Key key{output, workspace};
    auto leaf = std::make_unique<Node>();
    leaf->window = window;
    Node *added = leaf.get();
    auto &root = tiling->roots[key];
    if (!root) {
        root = std::move(leaf);
        tiling->leaves[window] = {added, key};
        return;
    }
    Node *split = nullptr;
    if (auto found = tiling->leaves.find(target);
        target && found != tiling->leaves.end() && found->second.second == key)
        split = found->second.first;
    if (!split && has_point)
        split = leaf_at(root.get(), x, y);
    if (!split) // New windows take the second half, so the newest leaf ends the chain.
        for (split = root.get(); !split->window; split = split->children[1].get())
            ;
    const sh_rect box = split->box;
    bool first = false;
    if (has_point && contains(box, x, y))
        first =
            splits_horizontally(box) ? x < box.x + box.width / 2.0 : y < box.y + box.height / 2.0;
    auto &slot = tiling->slot(split, key);
    auto parent = std::make_unique<Node>();
    parent->parent = split->parent;
    parent->box = box;
    parent->horizontal = splits_horizontally(box);
    leaf->box = box;
    split->parent = leaf->parent = parent.get();
    parent->children[first ? 1 : 0] = std::move(slot);
    parent->children[first ? 0 : 1] = std::move(leaf);
    slot = std::move(parent);
    tiling->leaves[window] = {added, key};
}

void sh_tiling_remove(sh_tiling *tiling, const void *window) {
    if (!tiling)
        return;
    auto found = tiling->leaves.find(window);
    if (found == tiling->leaves.end())
        return;
    auto [node, key] = found->second;
    tiling->leaves.erase(found);
    Node *parent = node->parent;
    if (!parent) {
        tiling->roots.erase(key);
        return;
    }
    // The sibling takes over the parent's place and box.
    auto sibling = std::move(parent->children[parent->children[0].get() == node ? 1 : 0]);
    sibling->parent = parent->parent;
    tiling->slot(parent, key) = std::move(sibling);
}

const char *sh_tiling_output(const sh_tiling *tiling, const void *window) {
    if (!tiling)
        return nullptr;
    auto found = tiling->leaves.find(window);
    return found == tiling->leaves.end() ? nullptr : found->second.second.first.c_str();
}

void sh_tiling_arrange(sh_tiling *tiling, const char *output, int workspace, sh_rect area, int gap,
                       sh_tile_place place, void *userdata) {
    if (!tiling || !output || !place || area.width < 1 || area.height < 1)
        return;
    auto found = tiling->roots.find({output, workspace});
    if (found == tiling->roots.end())
        return;
    gap = std::clamp(gap, 0, std::min(area.width, area.height) / 4);
    tiling->layout(found->second.get(), area, area, gap, place, userdata);
}

bool sh_tiling_preview(sh_tiling *tiling, const char *output, int workspace, void *window,
                       const void *target, bool has_point, double x, double y, sh_rect area,
                       int gap, sh_rect *result) {
    if (!tiling || !output || !window || !result || area.width < 1 || area.height < 1 ||
        tiling->leaves.contains(window))
        return false;
    struct Found {
        void *window;
        sh_rect *rect;
        bool found;
    } found{window, result, false};
    sh_tiling_insert(tiling, output, workspace, window, target, has_point, x, y);
    sh_tiling_arrange(
        tiling, output, workspace, area, gap,
        [](void *data, void *placed, sh_rect rect) {
            auto *found = static_cast<Found *>(data);
            if (placed == found->window) {
                *found->rect = rect;
                found->found = true;
            }
        },
        &found);
    sh_tiling_remove(tiling, window);
    // Arranging again restores the boxes that later insertions and resizes read.
    sh_tiling_arrange(
        tiling, output, workspace, area, gap, [](void *, void *, sh_rect) {}, nullptr);
    return found.found;
}

bool sh_tiling_resize(sh_tiling *tiling, const void *window, uint32_t edges, sh_rect rect) {
    if (!tiling)
        return false;
    auto found = tiling->leaves.find(window);
    if (found == tiling->leaves.end())
        return false;
    bool changed = false, horizontal_done = false, vertical_done = false;
    auto set = [&changed](Node *node, double position, int start, int length) {
        double ratio = std::clamp((position - start) / length, 0.1, 0.9);
        changed |= ratio != node->ratio;
        node->ratio = ratio;
    };
    for (Node *child = found->second.first, *node = child->parent; node;
         child = node, node = node->parent) {
        bool is_first = node->children[0].get() == child;
        const sh_rect &box = node->box;
        if (node->horizontal && !horizontal_done && box.width > 0) {
            if ((edges & SH_EDGE_RIGHT) && is_first) {
                set(node, rect.x + rect.width, box.x, box.width);
                horizontal_done = true;
            } else if ((edges & SH_EDGE_LEFT) && !is_first) {
                set(node, rect.x, box.x, box.width);
                horizontal_done = true;
            }
        } else if (!node->horizontal && !vertical_done && box.height > 0) {
            if ((edges & SH_EDGE_BOTTOM) && is_first) {
                set(node, rect.y + rect.height, box.y, box.height);
                vertical_done = true;
            } else if ((edges & SH_EDGE_TOP) && !is_first) {
                set(node, rect.y, box.y, box.height);
                vertical_done = true;
            }
        }
    }
    return changed;
}
}
