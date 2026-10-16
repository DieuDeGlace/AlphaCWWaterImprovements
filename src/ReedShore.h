#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
struct ShoreCell {
    bool valid = false;
    int ground = -10000, groundType = 0, water = -10000, depth = 0;
    bool rock = false;
    int bottom = -10000; // Lowest stored cell of the column; below it is solid.
};
static bool dryBank(const ShoreCell &cell) {
    return cell.valid && !cell.rock && cell.depth == 0 && cell.groundType == 4;
}
static bool reedPatchCell(int x, int y) {
    int tx = x / 10, ty = y / 10;
    uint32_t hash = uint32_t(tx) * 73856093u ^ uint32_t(ty) * 19349663u;
    int ax = tx * 10 + 3 + int(hash % 3), ay = ty * 10 + 3 + int((hash >> 8) % 3);
    int dx = x - ax, dy = y - ay;
    return dx * dx + dy * dy <= 9;
}
// Offsets within the 48-block search square, nearest first.
static const std::vector<std::pair<int, int>> &reedSearchOrder() {
    static const std::vector<std::pair<int, int>> order = [] {
        std::vector<std::pair<int, int>> o;
        for (int dy = -48; dy <= 48; dy++)
            for (int dx = -48; dx <= 48; dx++)
                o.push_back({dx, dy});
        std::stable_sort(o.begin(), o.end(), [](auto &a, auto &b) {
            return a.first * a.first + a.second * a.second <
                   b.first * b.first + b.second * b.second;
        });
        return o;
    }();
    return order;
}
// Nearest bank for a clump standing in water. First choice: shoreline patch
// cells (irregular clusters about six blocks across). When no patch site is
// free, any dry grass bank beside the water. Cells are visited nearest first
// and the search stops once no farther cell can beat the best score.
template <class Cell, class Occupied>
static bool chooseReedBank(int x, int y, int chunkX, int chunkY, Cell cell, Occupied occupied,
                           int &outX, int &outY, int &outZ) {
    const auto source = cell(x, y);
    if (!source.valid || source.depth < 1)
        return false;
    int best[2] = {0x7fffffff, 0x7fffffff}, found[2][3]{};
    for (auto &offset : reedSearchOrder()) {
        int distance = offset.first * offset.first + offset.second * offset.second;
        // Patch sites win at any distance; once one is found, nothing farther
        // can beat it (the score is at least distance * 4).
        if (distance * 4 > best[0])
            break;
        int nx = x + offset.first, ny = y + offset.second;
        bool patch = reedPatchCell(nx, ny);
        if (!patch && best[1] != 0x7fffffff)
            continue; // Only patch sites can still improve the result.
        if (occupied(nx, ny))
            continue;
        auto c = cell(nx, ny);
        if (!dryBank(c) || c.ground < source.water - 8 || c.ground > source.water + 8)
            continue;
        bool shore = false;
        for (int dy = -4; dy <= 4 && !shore; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                auto q = cell(nx + dx, ny + dy);
                if (q.valid && q.depth > 0 && std::abs(q.water - c.ground) <= 8) {
                    shore = true;
                    break;
                }
            }
        if (!shore)
            continue;
        int score = distance * 4 + std::abs(c.ground - source.water) * 2;
        for (int tier = patch ? 0 : 1; tier < 2; tier++)
            if (score < best[tier]) {
                best[tier] = score;
                found[tier][0] = nx;
                found[tier][1] = ny;
                found[tier][2] = c.ground + 1;
            }
    }
    int tier = best[0] != 0x7fffffff ? 0 : best[1] != 0x7fffffff ? 1 : -1;
    if (tier < 0)
        return false;
    outX = found[tier][0];
    outY = found[tier][1];
    outZ = found[tier][2];
    return true;
}
