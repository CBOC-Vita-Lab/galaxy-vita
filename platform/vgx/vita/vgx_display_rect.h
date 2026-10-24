#pragma once

namespace vgxvita {
struct DisplayRect {
    int x, y, width, height;
};

inline DisplayRect GetDisplayRect(bool stretch) {
    const int width = stretch ? 960 : 544 * 4 / 3;
    return {(960 - width) / 2, 0, width, 544};
}
} // namespace vgxvita
