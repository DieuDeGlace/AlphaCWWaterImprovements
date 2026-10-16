#include "LoadedChunkWork.h"
#include <cassert>
#include <cstdio>
int main() {
    LoadedChunkWork work;
    work.begin();
    work.observe(1, 0, 10);
    work.observe(2, 0, 20);
    work.end();
    int x = -1, y = -1;
    auto *near = work.next(0, 0, 100, x, y);
    assert(near && x == 1);
    near->cursor = 128;
    work.begin();
    work.observe(1, 0, 10);
    work.observe(2, 0, 20);
    work.end();
    assert(!work.changed && work.next(0, 0, 150, x, y)->cursor == 128);
    near->completed = 150;
    near->cursor = 0;
    auto *far = work.next(0, 0, 200, x, y);
    assert(far && x == 2);
    far->completed = 200;
    assert(!work.next(0, 0, 201, x, y));
    work.begin();
    work.observe(1, 0, 11); // Same coordinates, replacement terrain.
    work.end();
    assert(work.changed && work.chunks.size() == 1);
    assert(work.next(0, 0, 250, x, y)->completed == 0);
    work.chunks.begin()->second.completed = 250;
    assert(work.next(0, 0, 5250, x, y)); // Bounded regeneration fallback.
    // Fallback revisits stay near the player; new chunks are found anywhere.
    work.begin();
    work.observe(1, 0, 11);
    work.observe(9, 0, 90);
    work.end();
    work.chunks[{1, 0}].completed = 6000;
    work.chunks[{9, 0}].completed = 6000;
    assert(!work.next(-5, 0, 12000, x, y, 3) && work.next(8, 0, 12000, x, y, 3) && x == 9);
    work.begin();
    work.observe(1, 0, 11);
    work.observe(9, 0, 90);
    work.observe(30, 0, 300); // Far but newly loaded: still before revisits.
    work.end();
    assert(work.next(0, 0, 12000, x, y, 3) && x == 30);
    // A load or regeneration reopens reed placement around it.
    work.chunks[{9, 0}].reedsSettled = true;
    work.chunks[{1, 0}].reedsSettled = true;
    work.begin();
    work.observe(1, 0, 11);
    work.observe(9, 0, 90);
    work.observe(30, 0, 300);
    work.observe(10, 0, 100);
    work.end();
    bool neighborReopened = !work.chunks[{9, 0}].reedsSettled;
    bool distantKept = work.chunks[{1, 0}].reedsSettled;
    assert(neighborReopened && distantKept);
    puts(
        "PASS: nearest-first loading, resumable cursor, replacement, bounded revisits, reed reopen");
}
