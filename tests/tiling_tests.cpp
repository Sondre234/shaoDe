#include "shaode/backend.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
using Placement = std::map<void *, sh_rect>;
Placement arrange(sh_tiling *tiling, sh_rect area, int gap, const char *output = "DP-1",
                  int workspace = 0) {
    Placement placed;
    sh_tiling_arrange(
        tiling, output, workspace, area, gap,
        [](void *data, void *window, sh_rect rect) {
            (*static_cast<Placement *>(data))[window] = rect;
        },
        &placed);
    return placed;
}
bool overlaps(const sh_rect &a, const sh_rect &b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
           b.y < a.y + a.height;
}
} // namespace

int main() {
    try {
        const sh_rect area{-1920, 20, 1920, 1040};
        int windows[64];
        sh_tiling *tiling = sh_tiling_create();
        require(tiling, "cannot create tiling");
        require(arrange(tiling, area, 8).empty(), "empty tree placed windows");

        sh_tiling_insert(tiling, "DP-1", 0, &windows[0], nullptr, false, 0, 0);
        auto placed = arrange(tiling, area, 8);
        require(placed.size() == 1, "single window missing");
        const sh_rect full = placed[&windows[0]];
        require(full.x == area.x + 8 && full.y == area.y + 8 && full.width == area.width - 16 &&
                    full.height == area.height - 16,
                "single window does not fill the area inside the gap");

        // A landscape split puts the new window on the right, with one gap between.
        sh_tiling_insert(tiling, "DP-1", 0, &windows[1], nullptr, false, 0, 0);
        placed = arrange(tiling, area, 8);
        sh_rect left = placed[&windows[0]], right = placed[&windows[1]];
        require(left.y == right.y && left.height == right.height, "first split not side by side");
        require(left.x + left.width + 8 == right.x, "inner gap wrong");
        require(right.x + right.width + 8 == area.x + area.width, "outer gap wrong");

        // The right half is portrait, so the third window goes below the second.
        sh_tiling_insert(tiling, "DP-1", 0, &windows[2], nullptr, false, 0, 0);
        placed = arrange(tiling, area, 8);
        require(placed[&windows[2]].x == placed[&windows[1]].x &&
                    placed[&windows[2]].y > placed[&windows[1]].y,
                "dwindle did not alternate the split direction");

        for (int count = 4; count <= 12; ++count) {
            sh_tiling_insert(tiling, "DP-1", 0, &windows[count - 1], nullptr, false, 0, 0);
            placed = arrange(tiling, area, 8);
            require(static_cast<int>(placed.size()) == count, "window lost");
            for (auto &[window, rect] : placed) {
                require(rect.width > 0 && rect.height > 0, "empty tile");
                require(rect.x >= area.x && rect.y >= area.y &&
                            rect.x + rect.width <= area.x + area.width &&
                            rect.y + rect.height <= area.y + area.height,
                        "tile outside the area");
                for (auto &[other, other_rect] : placed)
                    require(window == other || !overlaps(rect, other_rect), "tiles overlap");
            }
        }
        require(std::string(sh_tiling_output(tiling, &windows[5])) == "DP-1", "output lost");
        for (int i = 11; i >= 1; --i)
            sh_tiling_remove(tiling, &windows[i]);
        require(!sh_tiling_output(tiling, &windows[1]), "removed window still tiled");
        placed = arrange(tiling, area, 8);
        require(placed.size() == 1 && placed[&windows[0]].width == full.width &&
                    placed[&windows[0]].height == full.height,
                "removal did not give the space back");

        // A point in the left half of a landscape window puts the new window on its left.
        sh_tiling_insert(tiling, "DP-1", 0, &windows[1], &windows[0], true, area.x + 100, 500);
        placed = arrange(tiling, area, 8);
        require(placed[&windows[1]].x < placed[&windows[0]].x, "point did not choose the side");
        // Without a target, the point picks the window it is over.
        sh_tiling_insert(tiling, "DP-1", 0, &windows[2], nullptr, true, area.x + 100, 1000);
        placed = arrange(tiling, area, 8);
        require(placed[&windows[2]].x == placed[&windows[1]].x &&
                    placed[&windows[2]].y > placed[&windows[1]].y,
                "point did not choose the window under it");
        require(placed[&windows[0]].height == full.height, "unrelated window was split");

        // Dragging the right edge of the left column moves the split under the pointer.
        sh_rect column = placed[&windows[1]];
        column.width = area.x + 1200 - column.x;
        require(sh_tiling_resize(tiling, &windows[1], SH_EDGE_RIGHT, column), "resize ignored");
        placed = arrange(tiling, area, 8);
        require(std::abs(placed[&windows[0]].x - (area.x + 1200 + 4)) <= 4,
                "split did not follow the resized edge");
        require(!sh_tiling_resize(tiling, &windows[0], SH_EDGE_RIGHT, placed[&windows[0]]),
                "the outer edge of the output moved");

        // Trees are separate per output and workspace.
        sh_tiling_insert(tiling, "DP-1", 1, &windows[3], nullptr, false, 0, 0);
        sh_tiling_insert(tiling, "HDMI-A-1", 0, &windows[4], nullptr, false, 0, 0);
        require(arrange(tiling, area, 8, "DP-1", 1).size() == 1, "workspace trees shared");
        require(arrange(tiling, area, 8, "HDMI-A-1", 0).size() == 1, "output trees shared");
        require(arrange(tiling, area, 8).size() == 3, "other trees changed this one");
        sh_tiling_insert(tiling, "DP-1", 0, &windows[3], nullptr, false, 0, 0);
        require(arrange(tiling, area, 8).size() == 3, "a window was tiled twice");

        // Tiny areas still produce positive sizes.
        placed = arrange(tiling, {0, 0, 3, 2}, 100);
        for (auto &[window, rect] : placed)
            require(rect.width > 0 && rect.height > 0, "tiny area produced an empty tile");
        sh_tiling_destroy(tiling);
        std::cout << "Dwindle tiling splits, removal, pointer placement, and resizing passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
