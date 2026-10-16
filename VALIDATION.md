# Validation

2026-10-16 update. Built with MSVC 14.44 x86 C++17 `/O2 /MT /EHsc`.

DLL: `AlphaWater.dll`

SHA-256: `1a1d8c7370ac4445b31406586889d027c6bd552b6dccaaf7c47c72373142f78f`

WaterSelfTest, ReedShoreTest, ReedOwnershipTest, and LoadedChunkWorkTest pass. Scheduler tests cover priority, partial progress, replacement at the same coordinates, unload eviction, idle suppression, fallback revisits limited to chunks near the player, retry after an unreadable tile, and reopening reed placement around newly loaded chunks.

Live timing in game is not yet verified. The previous build's log showed 128-column batches about 60 ms apart and eight reed clumps per pass about 750 ms apart; this build removes those per-pass ceilings and intervals.
