#include "LoadedChunkWork.h"
#include <cassert>
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
}
