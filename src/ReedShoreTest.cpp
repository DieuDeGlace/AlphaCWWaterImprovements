#include "ReedShore.h"
#include <cassert>
#include <set>
#include <cstdio>
int main() {
    auto cell = [](int x, int y) {
        if (y <= 8)
            return ShoreCell{true, 2, 6, 9, 7};
        return ShoreCell{
            true, 10 + (y / 4) % 2, 4, -10000, 0, x >= 10 && x <= 18 && y >= 9 && y <= 17};
    };
    std::set<std::pair<int, int>> used;
    auto occupied = [&](int x, int y) {
        for (auto p : used) {
            int dx = x - p.first, dy = y - p.second;
            if (dx * dx + dy * dy < 1.1f * 1.1f)
                return true;
        }
        return false;
    };
    for (int i = 0; i < 8; i++) {
        int x, y, z;
        assert(chooseReedBank(12, 8, 0, 0, cell, occupied, x, y, z));
        assert(reedPatchCell(x, y));
        assert(dryBank(cell(x, y)));
        assert(y <= 12);
        assert(z == cell(x, y).ground + 1);
        used.insert({x, y});
    }
    assert(!dryBank(ShoreCell{true, 10, 6, -10000, 0}));
    assert(!dryBank(ShoreCell{true, 10, 7, -10000, 0}));
    auto raised = [&](int x, int y) {
        auto c = cell(x, y);
        if (x == 12 && y == 8)
            c.water = 17;
        return c;
    };
    int ax, ay, az;
    assert(chooseReedBank(12, 8, 0, 0, raised, [](int, int) { return false; }, ax, ay, az));
    int x, y, z;
    assert(!chooseReedBank(12, 20, 0, 0, cell, occupied, x, y, z));
    auto rocks = [](int, int) { return ShoreCell{true, 10, 4, -10000, 0, true}; };
    assert(!chooseReedBank(12, 8, 0, 0, rocks, occupied, x, y, z));
    // Fallback: when every patch site is taken, any free dry grass bank beside
    // the water is used, nearest first; patch sites still win when free.
    auto patchesTaken = [&](int px, int py) { return reedPatchCell(px, py); };
    int fx, fy, fz;
    assert(chooseReedBank(12, 8, 0, 0, cell, patchesTaken, fx, fy, fz));
    assert(!reedPatchCell(fx, fy) && dryBank(cell(fx, fy)));
    assert((fx - 12) * (fx - 12) + (fy - 8) * (fy - 8) <= 10); // (9, 9): nearest past the rocks.
    int px2, py2, pz2;
    assert(chooseReedBank(12, 8, 0, 0, cell, [](int, int) { return false; }, px2, py2, pz2));
    assert(reedPatchCell(px2, py2));
    // No grass bank at all (a lake ringed by sand): nothing to choose, so the
    // worker sinks the clump under the riverbed once the area is loaded.
    auto sand = [](int x, int y) {
        if (y <= 8)
            return ShoreCell{true, 2, 6, 9, 7};
        return ShoreCell{true, 10, 5, -10000, 0};
    };
    assert(!chooseReedBank(12, 8, 0, 0, sand, [](int, int) { return false; }, fx, fy, fz));
    puts("PASS: 8 reeds in shoreline patches, rock exclusion, sloping soil, spacing, dry-source "
         "rejection, nearest non-patch bank fallback, no-bank rejection");
}
