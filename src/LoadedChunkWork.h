#pragma once
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

// Poll only native cache slots, then drain newly loaded chunks before maintenance.
// A cursor survives a full write batch, so dense water cannot starve later columns.
struct LoadedChunkWork {
    struct State {
        uint64_t identity{}, seen{}, completed{}, retry{};
        unsigned cursor{};
        // Reed clumps here are ashore or have no site; cleared when this chunk
        // or a neighbor (whose bank cells it may use) loads or regenerates.
        bool reedsSettled{};
    };
    std::map<std::pair<int, int>, State> chunks;
    std::vector<std::pair<int, int>> fresh; // New or regenerated this pass.
    uint64_t generation{};
    bool changed{};
    void begin() {
        ++generation;
        changed = false;
        fresh.clear();
    }
    void observe(int x, int y, uint64_t identity) {
        auto &s = chunks[{x, y}];
        if (s.identity != identity) {
            s = {};
            s.identity = identity;
            changed = true;
            fresh.push_back({x, y});
        }
        s.seen = generation;
    }
    void end(int neighborRadius = 2) {
        for (auto it = chunks.begin(); it != chunks.end();) {
            if (it->second.seen != generation)
                it = chunks.erase(it);
            else
                ++it;
        }
        for (auto &f : fresh)
            for (int dy = -neighborRadius; dy <= neighborRadius; ++dy)
                for (int dx = -neighborRadius; dx <= neighborRadius; ++dx) {
                    auto it = chunks.find({f.first + dx, f.second + dy});
                    if (it != chunks.end())
                        it->second.reedsSettled = false;
                }
    }
    // Unvisited chunks first (nearest first, anywhere in the cache); completed
    // chunks are revisited after five seconds only within `revisit` chunks.
    State *next(int px, int py, uint64_t now, int &x, int &y, int revisit = INT_MAX) {
        State *best = nullptr;
        int bestDistance = 0;
        for (auto &item : chunks) {
            auto &s = item.second;
            int dx = item.first.first - px, dy = item.first.second - py;
            if (now < s.retry) // Terrain was briefly unreadable; others go first.
                continue;
            if (s.completed &&
                (now - s.completed < 5000 || std::abs(dx) > revisit || std::abs(dy) > revisit))
                continue;
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
