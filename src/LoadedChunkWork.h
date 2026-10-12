#pragma once
#include <cstdint>
#include <map>
#include <utility>

// Poll only native cache slots, then drain newly loaded chunks before maintenance.
// A cursor survives a full write batch, so dense water cannot starve later columns.
//
// Usage per worker tick: begin(); observe() every loaded chunk; end(); then
// next() picks the chunk to process.
struct LoadedChunkWork {
    struct State {
        uint64_t identity{};  // Changes when the chunk is (re)loaded or regenerated.
        uint64_t seen{};      // Generation in which the chunk was last observed.
        uint64_t completed{}; // Tick when the last full pass finished (0 = pending).
        unsigned cursor{};    // Next column (0..1023) to examine.
    };
    std::map<std::pair<int, int>, State> chunks;
    uint64_t generation{};
    bool changed{}; // A new or regenerated chunk was observed this generation.

    void begin() {
        ++generation;
        changed = false;
    }
    void observe(int x, int y, uint64_t identity) {
        auto &s = chunks[{x, y}];
        if (s.identity != identity) {
            s = {};
            s.identity = identity;
            changed = true;
        }
        s.seen = generation;
    }
    // Forgets chunks that were not observed this generation (unloaded).
    void end() {
        for (auto it = chunks.begin(); it != chunks.end();) {
            if (it->second.seen != generation)
                it = chunks.erase(it);
            else
                ++it;
        }
    }
    // Picks the next chunk to work on: never-completed chunks first, then the
    // ones nearest to (px, py). Completed chunks are revisited after 5 s.
    // Returns nullptr when nothing is due.
    State *next(int px, int py, uint64_t now, int &x, int &y) {
        State *best = nullptr;
        int bestDistance = 0;
        for (auto &item : chunks) {
            auto &s = item.second;
            if (s.completed && now - s.completed < 5000)
                continue;
            int dx = item.first.first - px, dy = item.first.second - py;
            int distance = dx * dx + dy * dy;
            if (!best || (best->completed && !s.completed) ||
                (bool(best->completed) == bool(s.completed) && distance < bestDistance)) {
                best = &s;
                bestDistance = distance;
                x = item.first.first;
                y = item.first.second;
            }
        }
        return best;
    }
};
