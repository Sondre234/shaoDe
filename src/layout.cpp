// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaode/backend.h"
#include <algorithm>
#include <cmath>

extern "C" bool sh_placement(sh_action action, sh_rect area, int gap, int index, int count,
                             sh_rect *result) {
    if (!result || area.width < 1 || area.height < 1 || gap < 0 || index < 0 || count < 1 ||
        index >= count)
        return false;
    if (action == SH_MAXIMIZE) {
        *result = area;
        return true;
    }
    int columns = 2, rows = 1, column = 0, row = 0;
    if (action == SH_SNAP_RIGHT)
        column = 1;
    else if (action == SH_TILE) {
        columns = static_cast<int>(std::ceil(std::sqrt(count)));
        rows = (count + columns - 1) / columns;
        column = index % columns;
        row = index / columns;
    } else if (action != SH_SNAP_LEFT)
        return false;
    if (area.width < columns || area.height < rows)
        return false;
    gap =
        std::min({gap, (area.width - columns) / (columns + 1), (area.height - rows) / (rows + 1)});
    const int width = area.width - (columns + 1) * gap;
    const int height = area.height - (rows + 1) * gap;
    const int left = width * column / columns;
    const int right = width * (column + 1) / columns;
    const int top = height * row / rows;
    const int bottom = height * (row + 1) / rows;
    *result = {area.x + gap * (column + 1) + left, area.y + gap * (row + 1) + top, right - left,
               bottom - top};
    return true;
}
