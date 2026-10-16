# Validation

2026-10-16 update. Built with MSVC 14.44 x86 C++17 `/O2 /MT /EHsc`.

DLL: `AlphaWater.dll`

SHA-256: `e5765d4d1034deda7acc5e920770c5b8e2980cc3f29cf642ad48d6f69dbf3f05`

WaterSelfTest, ReedShoreTest, ReedOwnershipTest, and LoadedChunkWorkTest pass. Scheduler tests cover priority, partial progress, replacement at the same coordinates, unload eviction, idle suppression, fallback revisits limited to chunks near the player, retry after an unreadable tile, and reopening reed placement around newly loaded chunks.

Live timing in game is not yet verified. The previous build's log showed 128-column batches about 60 ms apart and eight reed clumps per pass about 750 ms apart; this build removes those per-pass ceilings and intervals.

Reed fallback update (2026-10-16): the first in-game run of the faster build logged up to 362 clumps per pass with no unoccupied dry bank in a shoreline patch; those stayed standing in water. ReedShoreTest now also covers the nearest non-patch grass bank fallback and the no-bank case (which the worker resolves by sinking the clump under the riverbed once its search area is loaded). Live appearance of the fallback is not yet verified.
