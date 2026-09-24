// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaode/backend.h"
#include <iostream>
#include <stdexcept>
#include <vector>

void require(bool value) {
    if (!value)
        throw std::runtime_error("layout invariant failed");
}
int main() {
    try {
        const sh_rect area{-1920, 20, 1919, 1079};
        for (int count = 1; count < 64; ++count) {
            std::vector<sh_rect> placed;
            for (int i = 0; i < count; ++i) {
                sh_rect rect;
                require(sh_placement(SH_TILE, area, 8, i, count, &rect));
                require(rect.width > 0 && rect.height > 0);
                require(rect.x >= area.x && rect.y >= area.y);
                require(rect.x + rect.width <= area.x + area.width);
                require(rect.y + rect.height <= area.y + area.height);
                for (const auto &other : placed)
                    require(rect.x + rect.width <= other.x || other.x + other.width <= rect.x ||
                            rect.y + rect.height <= other.y || other.y + other.height <= rect.y);
                placed.push_back(rect);
            }
        }
        sh_rect left, right;
        require(sh_placement(SH_SNAP_LEFT, area, 8, 0, 1, &left));
        require(sh_placement(SH_SNAP_RIGHT, area, 8, 0, 1, &right));
        require(left.x + left.width + 8 == right.x);
        require(right.x + right.width + 8 == area.x + area.width);
        require(sh_placement(SH_TILE, {0, 0, 2, 2}, 100, 3, 4, &left));
        require(left.width == 1 && left.height == 1);
        require(!sh_placement(SH_TILE, {0, 0, 1, 1}, 0, 1, 2, &left));
        require(!sh_placement(SH_TILE, area, 8, 0, 0, &left));
        std::cout << "Tiling bounds, gaps, and non-overlap passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
