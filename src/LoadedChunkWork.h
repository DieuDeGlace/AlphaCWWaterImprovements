#pragma once
#include <cstdint>
#include <map>
#include <utility>

// Poll only native cache slots, then drain newly loaded chunks before maintenance.
// A cursor survives a full write batch, so dense water cannot starve later columns.
struct LoadedChunkWork {
    struct State {
        uint64_t identity{}, seen{}, completed{};
        unsigned cursor{};
    };
    std::map<std::pair<int, int>, State> chunks;
    uint64_t generation{};
    bool changed{};
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
    void end() {
        for (auto it = chunks.begin(); it != chunks.end();) {
            if (it->second.seen != generation)
                it = chunks.erase(it);
            else
                ++it;
        }
    }
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
