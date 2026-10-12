# Validation

2026-10-12 update. Built MSVC x86 C++17 `/O2 /MT /EHsc`.

DLL: `AlphaWater.dll`

SHA-256: `2b158c8a2783d48e4eac0b96c5ba0589242da22a7a42ed82d979d6b310d0abd2`

WaterSelfTest, ReedShoreTest, ReedOwnershipTest, and LoadedChunkWorkTest pass. Scheduler tests cover priority, partial progress, replacement at the same coordinates, unload eviction, idle suppression, and fallback revisits. Live load timing is not yet verified.
