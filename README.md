# AlphaCWWaterImprovements

Swimmable river water and safe shoreline reed relocation for Cube World Alpha.

Made by DieuDeGlace with AI assistance from OpenAI ChatGPT/Codex. MIT licensed. No game executable, game assets, saves, or runtime journals are included.

## Build

Install Visual Studio 2022 Build Tools with Desktop development with C++ and
the Windows SDK. Open an **x86 Native Tools Command Prompt for VS 2022**.
These Alpha DLLs are 32-bit; an x64 build will not load into Cube.exe.

```bat
git clone https://github.com/DieuDeGlace/AlphaCWWaterImprovements.git
cd AlphaCWWaterImprovements
build.cmd
```

The DLL appears under `build/Mods/`. No toolchain or game files are bundled.
Builds use C++17, /O2 /MT /EHsc, Unicode, and the Windows SDK. clang-cl in an
x86 MSVC environment may be substituted for cl. Test programs are built into
`build/tests/`; run them with Alpha and other exclusive-fullscreen games closed.
GPU tests require a working Windows D3D9 driver and do not measure in-game FPS.

## Install

Close Alpha, extract the release ZIP into the Alpha game folder, and launch
through the Alpha mod loader. The ZIP's `Mods` folder must merge with the
game's existing `Mods` folder. Keep the included configuration/support folders.
The repository name is the project name; the established DLL filename stays
unchanged for compatibility. Remove the DLL with the game closed to uninstall.

## Water and reeds

An external hidden 32-bit worker changes solid water type 3 to native liquid type 2. Simple river columns use two blocks of depth on height changes and three on flatter water, bounded by the column allocation. Mixed columns change only water cells. Other material types, including stone, remain unchanged. Terrain originals are journaled, writes validated with readback, and affected meshes marked dirty. The active water scan retains its existing 64-block radius and 128-column batch limit.

Underwater reed extensions have been removed entirely. No reed rendering hook or additional root draw is installed. The worker identifies native reed clumps as vegetation kind 22 and moves both copies of each placement record together. Original coordinates are journaled separately. It recognizes scenery-rock placements and stone terrain, avoids planting on rocks or occupied sites, and uses dry bank footing, including slopes and banks beside deeper water. Each native clump retains its existing appearance and animation.

Reed scans run every three seconds within 96 blocks, with at most eight clumps moved per pass. Candidates form irregular patches roughly six blocks across, with gaps between patches. Each destination must be grass (material 4) near water. Materials 6 and 7 are rejected. Existing non-grass or scenery-rock-buffer placements are rechecked and moved, as well as water reeds. Native rock placements have a conservative four-block exclusion buffer, and stone terrain is rejected. Clump centers stay at least 1.1 blocks apart inside patches. Candidates within 48 blocks may cross chunk boundaries. Both native foliage-list copies are transferred to the destination chunk; native list ownership follows the placement, without marking terrain dirty because that regenerates original vegetation. Link edits are validated and journaled with rollback. No clumps are added or deleted. If no valid site is available, the clump remains pending at its original position. Rock records, list links and terrain cells are not changed by relocation.

The worker validates both placement snapshots and list membership while briefly pausing the process, journals originals, applies coordinate-only changes and verifies readback. Native terrain and reed-position journals are local development/recovery data and are not distributed. Do not unload an active mod DLL or stop its worker during a terrain transaction. For normal uninstall, close Cube and remove Mods/AlphaWater.dll. Saved water changes are not automatically undone by removing the DLL.

Build build.cmd from an x86 Visual Studio developer prompt with Windows SDK. clang-cl targeting i686-pc-windows-msvc also works. WaterSelfTest checks water conversion and material preservation; ReedShoreTest.cpp checks safe bank selection, spacing and rejection cases. Source uses the included MIT license. No game assets are distributed.







Run `build\tests\ReedShoreTest.exe` and `build\tests\ReedOwnershipTest.exe`. The DLL also exports WaterSelfTest for a DLL test harness. Original native water/reed journals stay local and are not distributed. The release includes an empty water-port directory for local journals.

## Source formatting

The project uses the checked-in `.clang-format` style (four-space indentation and a 100-column target). From the repository root, run `clang-format -i src/*.cpp src/*.h` on the source files that exist. Keep third-party SMAA files and embedded shader strings unchanged.

The cleanup was rebuilt with MSVC for x86 and checked against the previous revision for identical executable tokens and shader strings. Runtime settings and installed DLLs are unchanged.
