// AlphaWater: turns Cube World Alpha's solid water blocks into native liquid
// water and moves reed clumps from the water onto nearby river banks.
//
// The DLL itself does almost nothing inside Cube.exe: DllMain launches a hidden
// `rundll32 AlphaWater.dll,WaterWorker <pid>` process. That external worker
// reads and patches the game's memory every 50 ms:
//   * step():             converts water columns in newly loaded 32x32 chunks.
//   * relocateReeds():     moves reed placement records onto dry grass banks.
//   * repairReedOwners():  moves a relocated reed's records into the chunk that
//                          now contains it.
// Every write is preceded by validation and a journal entry (in water-port\)
// while the game is briefly suspended, verified by readback, and rolled back
// on failure.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include "ReedShore.h"
#include "ReedOwnership.h"
#include "LoadedChunkWork.h"

using Bytes = std::vector<unsigned char>;

// ===========================================================================
// Cube World Alpha data layout
// ===========================================================================
//
// Terrain is stored as columns. Each column header is 32 bytes:
//   +16 int   base Z of the first cell
//   +24 DWORD pointer to the cells (4 bytes each: RGB + type byte)
//   +28 DWORD number of cells
// The low 5 bits of a cell's type byte are its material.

static constexpr int MATERIAL_MASK = 31;
static constexpr int MATERIAL_FLAGS = 224; // Upper 3 bits, preserved on conversion.
static constexpr int MATERIAL_AIR = 0;
static constexpr int MATERIAL_LIQUID_WATER = 2;
static constexpr int MATERIAL_SOLID_WATER = 3;

static constexpr DWORD COLUMN_HEADER_SIZE = 32;
static constexpr DWORD COLUMN_BASE_Z = 16, COLUMN_CELLS = 24, COLUMN_COUNT = 28;
static constexpr DWORD MAX_COLUMN_CELLS = 2048;

// Offsets from the game object (plus the per-build `layout` adjustment).
static constexpr DWORD GAME_PLAYER = 0x8006c8;
static constexpr DWORD GAME_REGIONS = 0x398; // 256x256 grid of region pointers.
static constexpr DWORD GAME_CHUNK_CACHE_WIDTH = 0x2d4;
static constexpr DWORD GAME_CHUNK_CACHE = 0x2d8;
static constexpr DWORD REGION_ZONES = 0x10018;  // 64x64 zone pointers per region.
static constexpr DWORD ZONE_COORDINATES = 0x60; // int x, y
static constexpr DWORD ZONE_COLUMNS = 0xa8;     // 256x256 column headers.

// Loaded-chunk cache entries (32x32 blocks each).
static constexpr DWORD CHUNK_STRIDE = 0x268;
static constexpr DWORD CHUNK_COORDINATES = 0x18; // int x, y
static constexpr DWORD CHUNK_MESH_DIRTY = 0x74;  // Byte; 1 = rebuild the mesh.
// Two copies of the chunk's vegetation list: head pointer at +0, count at +4,
// the second copy 8 bytes later.
static constexpr DWORD CHUNK_FLORA_LISTS = 0x240;
static constexpr DWORD CHUNK_FLORA_COUNTS = 0x244;
static constexpr DWORD CHUNK_SECOND_FLORA_LIST = 0x248;

// Vegetation records: next/prev links at +0/+4, kind at +8, int64 x/y/z
// (fixed point, 65536 per block) at +16.
static constexpr DWORD FLORA_RECORD_SIZE = 72;
static constexpr DWORD FLORA_KIND = 8, FLORA_POSITION = 16, FLORA_POSITION_SIZE = 24;
static constexpr DWORD FLORA_ROCK = 1, FLORA_REED = 22, FLORA_FIRST_RESERVED_KIND = 31;

static constexpr int SCAN_RADIUS = 96; // Blocks around the player.
static constexpr int CHUNK_SIZE = 32;  // Blocks per loaded chunk side.
static constexpr size_t MAX_REED_MOVES = 8;
static constexpr size_t MAX_COLUMN_CHANGES = 128;

// ===========================================================================
// Paths, logging and file hashing
// ===========================================================================

static HMODULE ownModule;
void InitializeAlphaReeds(HMODULE self);
extern "C" int __cdecl ReedSelfTest();

static std::wstring dllPath() {
    wchar_t b[32768];
    GetModuleFileNameW(ownModule, b, 32768);
    return b;
}
static std::wstring parent(std::wstring p) {
    return p.substr(0, p.find_last_of(L"\\/"));
}
// The game folder (the DLL lives in <game>\Mods).
static std::wstring root() {
    return parent(parent(dllPath()));
}
static std::wstring journalPath(const wchar_t *name) {
    return root() + L"\\water-port\\" + name;
}

static void log(const char *text) {
    FILE *f = nullptr;
    _wfopen_s(&f, journalPath(L"native-water.log").c_str(), L"a");
    if (f) {
        fprintf(f, "%llu %s\n", GetTickCount64(), text);
        fclose(f);
    }
}

// SHA-256 of a file as lowercase hex, or "" on failure.
static std::string digest(const std::wstring &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    std::string result;
    if (CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        unsigned char buffer[65536];
        DWORD got = 0;
        bool ok = true;
        while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got) {
            if (!CryptHashData(hash, buffer, got, 0)) {
                ok = false;
                break;
            }
        }
        unsigned char value[32];
        DWORD size = 32;
        if (ok && CryptGetHashParam(hash, HP_HASHVAL, value, &size, 0)) {
            char hex[3];
            for (auto b : value) {
                sprintf_s(hex, "%02x", b);
                result += hex;
            }
        }
    }
    if (hash)
        CryptDestroyHash(hash);
    if (provider)
        CryptReleaseContext(provider, 0);
    CloseHandle(file);
    return result;
}

// ===========================================================================
// Column transformations (pure functions, covered by WaterSelfTest)
// ===========================================================================

static int material(const Bytes &cells, size_t index) {
    return cells[index * 4 + 3] & MATERIAL_MASK;
}

// A "plain" water column: one solid-water cell on top of nothing but air.
static bool plainWater(const Bytes &b) {
    if (b.empty() || b.size() % 4 || (b[3] & MATERIAL_MASK) != MATERIAL_SOLID_WATER)
        return false;
    for (size_t i = 7; i < b.size(); i += 4)
        if (b[i] & MATERIAL_MASK)
            return false;
    return true;
}

// Turns a plain water column into `depth` cells of liquid water (copies of the
// original water cell), followed by copies of the second cell (air). Returns an
// empty vector for anything that is not plain water.
static Bytes deepen(const Bytes &b, int depth) {
    if (!plainWater(b))
        return {};
    int count = (int)b.size() / 4;
    depth = std::min(depth, count);
    Bytes out = b;
    for (int i = 0; i < count; ++i) {
        if (i < depth) {
            memcpy(out.data() + i * 4, b.data(), 4);
            out[i * 4 + 3] = (b[3] & MATERIAL_FLAGS) | MATERIAL_LIQUID_WATER;
        } else
            memcpy(out.data() + i * 4, b.data() + 4, 4);
    }
    return out;
}

// Changes every solid-water cell to liquid water and leaves other cells alone.
static Bytes liquidize(Bytes b) {
    for (size_t i = 3; i < b.size(); i += 4)
        if ((b[i] & MATERIAL_MASK) == MATERIAL_SOLID_WATER)
            b[i] = (b[i] & MATERIAL_FLAGS) | MATERIAL_LIQUID_WATER;
    return b;
}

// Reads a T at `offset`, or a value-initialised T when out of range.
template <class T> static T value(const Bytes &b, size_t offset) {
    T v{};
    if (offset + sizeof(T) <= b.size())
        memcpy(&v, b.data() + offset, sizeof(T));
    return v;
}

static int blockFromFixed(int64_t fixed) {
    return (int)(fixed / 65536);
}

// ===========================================================================
// Patch: one attached game process
// ===========================================================================

// A pending water conversion for one column.
struct Change {
    int x, y, surface, oldBase, newBase;
    DWORD column, data, count;
    Bytes before, after;
    std::vector<DWORD> flags; // Mesh-dirty flags of the chunks touching the column.
};

struct Patch {
    HANDLE process{};
    DWORD pid{}, base{}, dataRva{}, dataSize{};
    DWORD vtable{}; // Player creature vtable RVA for this build.
    DWORD game{};   // Game object, once located.
    DWORD layout{}; // Extra offset into the game object for this build.
    ULONGLONG lastFind{}, lastReeds{}, lastReedLog{}, lastOwnerRepair{};
    LoadedChunkWork loaded;
    bool reedBacklog = true;                      // Last reed pass hit the move limit.
    std::map<std::pair<int, int>, int> converted; // Column -> water surface height.
    using NtCall = LONG(NTAPI *)(HANDLE);
    NtCall suspend{}, resume{};

    ~Patch() {
        if (process)
            CloseHandle(process);
    }

    // ---- Remote memory access ------------------------------------------------

    Bytes read(DWORD address, size_t count) {
        if (!address || count > 16 * 1024 * 1024)
            return {};
        Bytes b(count);
        SIZE_T got = 0;
        if (!ReadProcessMemory(process, (LPCVOID)(uintptr_t)address, b.data(), count, &got) ||
            got != count)
            return {};
        return b;
    }
    DWORD ptr(DWORD a) {
        return value<DWORD>(read(a, 4), 0);
    }
    // Writes and verifies by reading back.
    bool write(DWORD address, const Bytes &b) {
        SIZE_T got = 0;
        return WriteProcessMemory(process, (LPVOID)(uintptr_t)address, b.data(), b.size(), &got) &&
               got == b.size() && read(address, b.size()) == b;
    }
    Bytes intBytes(int v) {
        Bytes b(4);
        memcpy(b.data(), &v, 4);
        return b;
    }

    // ---- Attaching ---------------------------------------------------------------

    // Opens the game process. Only known executables (by SHA-256) located in the
    // same game folder as this DLL are patched.
    bool init(DWORD id) {
        pid = id;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        MODULEENTRY32W m{};
        m.dwSize = sizeof(m);
        bool ok = Module32FirstW(snap, &m) != 0;
        CloseHandle(snap);
        if (!ok)
            return false;
        if (_wcsicmp(parent(m.szExePath).c_str(), root().c_str()))
            return false;
        std::string sha = digest(m.szExePath);
        if (sha == "84a7a132a84d4282338e7ea45a1940d32066d64e39cbafed8f2418d5a6dc30bf") {
            vtable = 0x2fd8cc;
            layout = 8;
        } else if (sha == "a4eeb3606ad2b82e4c9b3d0db6f9472ffa6e89a4d56084a9114ff1fb3c812699" ||
                   sha == "88798362b3516e339c4ee5f6defad2a8c36a5f0930a51e6a3e497b9531e382c6")
            vtable = 0x2fc8cc;
        else {
            log("Unsupported executable: no terrain changes made");
            return false;
        }
        base = (DWORD)(uintptr_t)m.modBaseAddr;
        // PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
        // PROCESS_SUSPEND_RESUME | PROCESS_QUERY_INFORMATION
        process = OpenProcess(0xC38, FALSE, pid);
        if (!process)
            return false;
        auto ntdll = GetModuleHandleW(L"ntdll.dll");
        suspend = (NtCall)GetProcAddress(ntdll, "NtSuspendProcess");
        resume = (NtCall)GetProcAddress(ntdll, "NtResumeProcess");
        if (!suspend || !resume)
            return false;
        // Locate the .data section, which is scanned for the game object.
        Bytes header = read(base, 4096);
        DWORD pe = value<DWORD>(header, 0x3c);
        WORD sections = value<WORD>(header, pe + 6), optional = value<WORD>(header, pe + 20);
        for (int i = 0; i < sections; ++i) {
            size_t o = pe + 24 + optional + i * 40;
            if (o + 40 > header.size())
                return false;
            if (!memcmp(header.data() + o, ".data\0\0\0", 8)) {
                dataSize = value<DWORD>(header, o + 8);
                dataRva = value<DWORD>(header, o + 12);
            }
        }
        return dataRva && dataSize && dataSize < 16 * 1024 * 1024;
    }

    // Reads the player's block position from game object `g`. Also serves as the
    // validity test for a candidate game object.
    bool player(DWORD g, int &x, int &y, int &z) {
        DWORD p = ptr(g + layout + GAME_PLAYER);
        Bytes b = read(p, 0x150);
        if (b.empty() || value<DWORD>(b, 0) != base + vtable)
            return false;
        for (int i = 0; i < 3; ++i) {
            float dim = value<float>(b, 0x80 + i * 4);
            if (!std::isfinite(dim) || dim <= .1 || dim >= 20)
                return false;
        }
        double xx = value<int64_t>(b, 16) / 65536.0, yy = value<int64_t>(b, 24) / 65536.0,
               zz = value<int64_t>(b, 32) / 65536.0;
        if (!(xx > 0 && xx < 16777216 && yy > 0 && yy < 16777216 && zz > -10000 && zz < 10000))
            return false;
        x = (int)std::floor(xx);
        y = (int)std::floor(yy);
        z = (int)std::floor(zz);
        return true;
    }

    // Finds the game object: the only pointer (in .data or one level below it)
    // whose player passes the checks in player().
    void find() {
        Bytes b = read(base + dataRva, dataSize);
        std::set<DWORD> candidates;
        auto add = [&](const Bytes &block) {
            for (size_t i = 0; i + 4 <= block.size(); i += 4) {
                DWORD p = value<DWORD>(block, i);
                if (p > 0x10000 && p < 0x7fff0000 && p % 4 == 0)
                    candidates.insert(p);
            }
        };
        add(b);
        auto first = candidates;
        for (DWORD p : first)
            add(read(p, 512));
        DWORD found = 0;
        int matches = 0, x, y, z;
        for (DWORD p : candidates)
            if (player(p, x, y, z)) {
                found = p;
                ++matches;
            }
        if (matches == 1) {
            game = found;
            log("Located active character");
        }
    }

    // True when the game object is known and the player can be read.
    bool playerPosition(int &x, int &y, int &z) {
        return game && player(game, x, y, z);
    }

    // ---- Terrain lookups -------------------------------------------------------

    // Address of the 256x256 column-header array of the zone containing (x, y), or 0.
    DWORD columns(int x, int y) {
        if (x < 0 || y < 0 || x >= 16777216 || y >= 16777216)
            return 0;
        int zx = x / 256, zy = y / 256;
        DWORD region = ptr(game + layout + GAME_REGIONS + ((zx / 64) * 1024 + zy / 64) * 4);
        DWORD zone = region ? ptr(region + REGION_ZONES + ((zx % 64) * 64 + zy % 64) * 4) : 0;
        Bytes xy = read(zone + ZONE_COORDINATES, 8);
        if (!zone || xy.size() != 8 || value<int>(xy, 0) != zx || value<int>(xy, 4) != zy)
            return 0;
        return ptr(zone + ZONE_COLUMNS);
    }
    static DWORD columnOffset(int x, int y) {
        return (x % 256 + y % 256 * 256) * COLUMN_HEADER_SIZE;
    }

    // The loaded-chunk cache: a width x width ring of 32x32-block chunks.
    struct ChunkCache {
        DWORD width = 0, cache = 0;
        bool valid() const {
            return cache && width >= 1 && width <= 64;
        }
        DWORD slot(int cx, int cy) const {
            return cache + ((cy % width) * width + cx % width) * CHUNK_STRIDE;
        }
    };
    ChunkCache chunkCache() {
        return {ptr(game + layout + GAME_CHUNK_CACHE_WIDTH), ptr(game + layout + GAME_CHUNK_CACHE)};
    }
    // The cache entry for chunk (cx, cy) if that chunk is currently loaded, else 0.
    DWORD loadedChunk(const ChunkCache &chunks, int cx, int cy) {
        DWORD chunk = chunks.slot(cx, cy);
        Bytes xy = read(chunk + CHUNK_COORDINATES, 8);
        return xy.size() == 8 && value<int>(xy, 0) == cx && value<int>(xy, 4) == cy ? chunk : 0;
    }

    // Mesh-dirty flags of every loaded chunk within one block of (x, y).
    std::vector<DWORD> flags(int x, int y) {
        ChunkCache chunks = chunkCache();
        std::vector<DWORD> out;
        if (!chunks.valid())
            return out;
        for (int cx = (x - 1) / CHUNK_SIZE; cx <= (x + 1) / CHUNK_SIZE; ++cx)
            for (int cy = (y - 1) / CHUNK_SIZE; cy <= (y + 1) / CHUNK_SIZE; ++cy)
                if (DWORD chunk = loadedChunk(chunks, cx, cy))
                    out.push_back(chunk + CHUNK_MESH_DIRTY);
        return out;
    }

    // Reads a column's base Z and cells. Returns false for missing/implausible columns.
    bool readColumn(int x, int y, int &baseZ, Bytes &cells) {
        DWORD cols = columns(x, y);
        if (!cols)
            return false;
        Bytes column = read(cols + columnOffset(x, y), COLUMN_HEADER_SIZE);
        DWORD count = value<DWORD>(column, COLUMN_COUNT);
        baseZ = value<int>(column, COLUMN_BASE_Z);
        if (count < 1 || count > MAX_COLUMN_CELLS)
            return false;
        cells = read(value<DWORD>(column, COLUMN_CELLS), count * 4);
        return cells.size() == count * 4;
    }

    // Height of the water surface at (x, y) for water-only columns.
    bool surface(int x, int y, int &height) {
        int bz;
        Bytes cells;
        if (!readColumn(x, y, bz, cells))
            return false;
        if (plainWater(cells)) {
            height = bz;
            return true;
        }
        int top = -1;
        DWORD count = (DWORD)cells.size() / 4;
        for (DWORD i = 0; i < count; ++i) {
            int t = material(cells, i);
            if (t != MATERIAL_AIR && t != MATERIAL_LIQUID_WATER)
                return false;
            if (t == MATERIAL_LIQUID_WATER)
                top = i;
        }
        if (top < 0)
            return false;
        height = bz + top;
        return true;
    }

    // Water depth for a converted column: 2 where a neighbour's surface differs
    // (slopes and river edges), otherwise 3.
    int depth(int x, int y, int height) {
        int h;
        for (auto d : std::array<std::pair<int, int>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}})
            if (surface(x + d.first, y + d.second, h) && h != height)
                return 2;
        return 3;
    }

    // Terrain summary used for reed placement.
    ShoreCell shoreCell(int x, int y) {
        ShoreCell out;
        int bz;
        Bytes cells;
        if (!readColumn(x, y, bz, cells))
            return out;
        DWORD count = (DWORD)cells.size() / 4;
        out.valid = true;
        int highestWater = -1;
        for (DWORD i = 0; i < count; i++) {
            int type = material(cells, i);
            if (type == MATERIAL_LIQUID_WATER || type == MATERIAL_SOLID_WATER) {
                highestWater = (int)i;
                out.water = bz + i;
            } else if (type) {
                out.ground = bz + i;
                out.groundType = type;
            }
        }
        if (highestWater >= 0) {
            for (int i = highestWater; i >= 0; i--) {
                int type = material(cells, i);
                if (type != MATERIAL_LIQUID_WATER && type != MATERIAL_SOLID_WATER)
                    break;
                out.depth++;
            }
            if (out.ground >= out.water) {
                out.depth = 0;
                out.water = -10000;
            }
        }
        return out;
    }

    // ---- Journals ----------------------------------------------------------------

    // Appends the original cells of a terrain batch to native-water-originals.bin.
    bool journal(const std::vector<Change> &batch) {
        FILE *f = nullptr;
        _wfopen_s(&f, journalPath(L"native-water-originals.bin").c_str(), L"ab");
        if (!f)
            return false;
        DWORD header[] = {0x41575031 /* "1PWA" */, pid, (DWORD)batch.size()};
        bool ok = fwrite(header, sizeof(header), 1, f) == 1;
        for (const auto &c : batch) {
            DWORD row[] = {(DWORD)c.x, (DWORD)c.y,       c.column,        c.data,
                           c.count,    (DWORD)c.oldBase, (DWORD)c.newBase};
            ok = ok && fwrite(row, sizeof(row), 1, f) == 1;
            ok = ok && fwrite(c.before.data(), c.before.size(), 1, f) == 1;
        }
        ok = ok && fflush(f) == 0;
        fclose(f);
        return ok;
    }

    // ---- Reed ownership repair -------------------------------------------------

    // Finds the single reed record in each of the source chunk's two flora lists
    // whose position matches `record`. Throws if a list is inconsistent.
    void findReedCopies(DWORD source, const Bytes &record, DWORD nodes[2]) {
        for (int copy = 0; copy < 2; copy++) {
            DWORD h = ptr(source + CHUNK_FLORA_LISTS + copy * 8), n = ptr(h);
            unsigned count = 0, matches = 0;
            std::set<DWORD> visited;
            while (n && n != h && visited.size() < 4096) {
                if (!visited.insert(n).second)
                    break;
                Bytes b = read(n, FLORA_RECORD_SIZE);
                if (b.size() != FLORA_RECORD_SIZE)
                    break;
                if (value<DWORD>(b, FLORA_KIND) == FLORA_REED &&
                    !memcmp(b.data() + FLORA_POSITION, record.data() + FLORA_POSITION,
                            FLORA_POSITION_SIZE)) {
                    nodes[copy] = n;
                    matches++;
                }
                n = value<DWORD>(b, 0);
                count++;
            }
            if (n != h || matches != 1 || count != ptr(source + CHUNK_FLORA_COUNTS + copy * 8))
                throw std::runtime_error("Native foliage list incomplete");
        }
    }

    // Moves the reed at `node`/`record` (and its twin in the second list) from the
    // source chunk's flora lists to the destination chunk's. Must be called with
    // the game suspended; resumes it. Always ends the repair pass.
    void transferReed(DWORD source, DWORD destination, DWORD head, DWORD node,
                      const Bytes &record) {
        std::map<DWORD, std::pair<Bytes, Bytes>> edits; // address -> (before, after)
        size_t applied = 0;
        try {
            if (read(node, FLORA_RECORD_SIZE) != record ||
                ptr(source + CHUNK_SECOND_FLORA_LIST) != head || !destination)
                throw std::runtime_error("Reed ownership changed before repair");
            DWORD nodes[2]{};
            findReedCopies(source, record, nodes);
            auto stage = [&](DWORD address, const Bytes &after) {
                auto old = read(address, after.size());
                if (old.size() != after.size())
                    throw std::runtime_error("Ownership read failed");
                auto it = edits.find(address);
                if (it == edits.end())
                    edits.emplace(address, std::make_pair(old, after));
                else
                    it->second.second = after;
            };
            for (int copy = 0; copy < 2; copy++) {
                DWORD sh = ptr(source + CHUNK_FLORA_LISTS + copy * 8),
                      dh = ptr(destination + CHUNK_FLORA_LISTS + copy * 8);
                transferReedLinks(
                    nodes[copy], sh, dh, source + CHUNK_FLORA_COUNTS + copy * 8,
                    destination + CHUNK_FLORA_COUNTS + copy * 8, [&](DWORD a) { return ptr(a); },
                    [&](DWORD a, DWORD v) { stage(a, intBytes(v)); });
            }
            // Preserve native vegetation: full terrain rebuild regenerates its original placements.
            FILE *journal = nullptr;
            if (_wfopen_s(&journal, journalPath(L"reed-ownership-originals.bin").c_str(), L"ab") ||
                !journal)
                throw std::runtime_error("Ownership journal unavailable");
            bool ok = true;
            DWORD header[] = {0x41524f31 /* "1ORA" */, pid, (DWORD)edits.size()};
            ok = fwrite(header, sizeof(header), 1, journal) == 1;
            for (auto &e : edits) {
                DWORD info[] = {e.first, (DWORD)e.second.first.size()};
                ok = ok && fwrite(info, sizeof(info), 1, journal) == 1 &&
                     fwrite(e.second.first.data(), e.second.first.size(), 1, journal) == 1 &&
                     fwrite(e.second.second.data(), e.second.second.size(), 1, journal) == 1;
            }
            ok = ok && fflush(journal) == 0;
            fclose(journal);
            if (!ok)
                throw std::runtime_error("Ownership journal write failed");
            for (auto &e : edits) {
                ++applied;
                if (!write(e.first, e.second.second))
                    throw std::runtime_error("Ownership write failed");
            }
            resume(process);
            log("Reed ownership transferred to bank chunk; native vegetation retained without terrain regeneration");
        } catch (const std::exception &e) {
            // Undo the edits that were written, in map order.
            size_t index = 0;
            for (auto &edit : edits) {
                if (index++ >= applied)
                    break;
                write(edit.first, edit.second.first);
            }
            resume(process);
            log(e.what());
        }
    }

    // Every 3 s: finds one reed whose position lies in a different chunk than the
    // one that owns its record and transfers ownership to that chunk.
    void repairReedOwners() {
        auto now = GetTickCount64();
        if (now - lastOwnerRepair < 3000)
            return;
        lastOwnerRepair = now;
        int px, py, pz;
        if (!playerPosition(px, py, pz))
            return;
        ChunkCache chunks = chunkCache();
        if (!chunks.valid())
            return;
        for (int cy = (py - SCAN_RADIUS) / CHUNK_SIZE; cy <= (py + SCAN_RADIUS) / CHUNK_SIZE; cy++)
            for (int cx = (px - SCAN_RADIUS) / CHUNK_SIZE; cx <= (px + SCAN_RADIUS) / CHUNK_SIZE;
                 cx++) {
                DWORD source = loadedChunk(chunks, cx, cy);
                if (!source)
                    continue;
                DWORD head = ptr(source + CHUNK_SECOND_FLORA_LIST), node = ptr(head);
                std::set<DWORD> seen;
                while (node && node != head && seen.size() < 4096) {
                    if (!seen.insert(node).second)
                        break;
                    Bytes record = read(node, FLORA_RECORD_SIZE);
                    if (record.size() != FLORA_RECORD_SIZE)
                        break;
                    DWORD next = value<DWORD>(record, 0);
                    int x = blockFromFixed(value<int64_t>(record, FLORA_POSITION)),
                        y = blockFromFixed(value<int64_t>(record, FLORA_POSITION + 8));
                    bool ownedByThisChunk = x / CHUNK_SIZE == cx && y / CHUNK_SIZE == cy;
                    if (value<DWORD>(record, FLORA_KIND) != FLORA_REED || ownedByThisChunk) {
                        node = next;
                        continue;
                    }
                    DWORD destination = loadedChunk(chunks, x / CHUNK_SIZE, y / CHUNK_SIZE);
                    if (!destination) {
                        node = next;
                        continue;
                    }
                    if (suspend(process) != 0)
                        return;
                    transferReed(source, destination, head, node, record);
                    return;
                }
            }
    }

    // ---- Reed relocation -------------------------------------------------------

    // One reed placement record (each clump appears in both flora lists).
    struct Plant {
        DWORD node, head, chunk;
        Bytes before;
        std::array<int64_t, 3> position;
    };
    struct Move {
        Plant first, second;
        Bytes destination; // New fixed-point x/y/z (24 bytes).
    };
    using PlantsByPosition = std::map<std::array<int64_t, 3>, std::vector<Plant>>;
    struct Group {
        int cx, cy;
        PlantsByPosition first, second;
    };

    // Everything learned about the vegetation around the player in one pass.
    struct FloraScan {
        std::vector<Group> groups;
        std::set<std::pair<int, int>> reserved;      // Cells already holding vegetation.
        std::set<std::pair<int, int>> rockLocations; // Rock records plus a 4-block buffer.
        std::set<std::pair<int, int>> reedLocations; // Reed clump centres.
    };

    // Walks both flora lists of every loaded chunk within SCAN_RADIUS, collecting
    // reed records and occupied/rock cells. Chunks whose lists cannot be read
    // completely are skipped.
    FloraScan scanFlora(const ChunkCache &chunks, int px, int py) {
        FloraScan scan;
        for (int cy = (py - SCAN_RADIUS) / CHUNK_SIZE; cy <= (py + SCAN_RADIUS) / CHUNK_SIZE; cy++)
            for (int cx = (px - SCAN_RADIUS) / CHUNK_SIZE; cx <= (px + SCAN_RADIUS) / CHUNK_SIZE;
                 cx++) {
                DWORD chunk = loadedChunk(chunks, cx, cy);
                if (!chunk)
                    continue;
                PlantsByPosition lists[2];
                bool complete = true;
                for (int copy = 0; copy < 2; copy++) {
                    DWORD head = ptr(chunk + CHUNK_FLORA_LISTS + copy * 8), node = ptr(head);
                    std::set<DWORD> seen;
                    while (node && node != head && seen.size() < 4096) {
                        if (!seen.insert(node).second) {
                            complete = false;
                            break;
                        }
                        Bytes b = read(node, FLORA_RECORD_SIZE);
                        if (b.size() != FLORA_RECORD_SIZE) {
                            complete = false;
                            break;
                        }
                        DWORD kind = value<DWORD>(b, FLORA_KIND);
                        std::array<int64_t, 3> position{value<int64_t>(b, FLORA_POSITION),
                                                        value<int64_t>(b, FLORA_POSITION + 8),
                                                        value<int64_t>(b, FLORA_POSITION + 16)};
                        int x = blockFromFixed(position[0]), y = blockFromFixed(position[1]);
                        if (kind == FLORA_REED)
                            lists[copy][position].push_back({node, head, chunk, b, position});
                        if (kind == FLORA_ROCK) {
                            for (int ry = -4; ry <= 4; ry++)
                                for (int rx = -4; rx <= 4; rx++)
                                    scan.rockLocations.insert({x + rx, y + ry});
                        }
                        if (kind == FLORA_REED)
                            scan.reedLocations.insert({x, y});
                        if (kind == FLORA_ROCK || kind == FLORA_REED ||
                            kind >= FLORA_FIRST_RESERVED_KIND)
                            scan.reserved.insert({x, y});
                        node = value<DWORD>(b, 0);
                    }
                    if (node != head)
                        complete = false;
                }
                if (!complete)
                    continue;
                scan.groups.push_back({cx, cy, std::move(lists[0]), std::move(lists[1])});
            }
        return scan;
    }

    // Validates, journals and applies a batch of reed moves while the game is
    // suspended. Returns the number of moves written. Throws on failure after
    // rolling back.
    size_t applyReedMoves(std::vector<Move> &moves) {
        if (suspend(process) != 0)
            throw std::runtime_error("Could not pause reed placements safely");
        size_t applied = 0;
        FILE *journal = nullptr;
        try {
            // Skip moves whose records changed since they were read.
            std::vector<Move> valid;
            for (auto &move : moves) {
                auto matches = [&](const Plant &p) {
                    return read(p.node, FLORA_RECORD_SIZE) == p.before &&
                           ptr(p.chunk + CHUNK_FLORA_LISTS +
                               (p.head == ptr(p.chunk + CHUNK_SECOND_FLORA_LIST) ? 8 : 0)) ==
                               p.head &&
                           ptr(value<DWORD>(p.before, 0) + 4) == p.node &&
                           ptr(value<DWORD>(p.before, 4)) == p.node;
                };
                if (matches(move.first) && matches(move.second))
                    valid.push_back(move);
            }
            if (!_wfopen_s(&journal, journalPath(L"reed-placement-originals.bin").c_str(), L"ab") &&
                journal) {
                DWORD header[] = {0x41525031 /* "1PRA" */, pid, (DWORD)valid.size()};
                if (fwrite(header, sizeof(header), 1, journal) != 1)
                    throw std::runtime_error("Reed journal failed");
                for (auto &move : valid) {
                    DWORD addresses[] = {move.first.node, move.second.node};
                    if (fwrite(addresses, sizeof(addresses), 1, journal) != 1 ||
                        fwrite(move.first.before.data() + FLORA_POSITION, FLORA_POSITION_SIZE, 1,
                               journal) != 1 ||
                        fwrite(move.destination.data(), FLORA_POSITION_SIZE, 1, journal) != 1)
                        throw std::runtime_error("Reed journal failed");
                }
                if (fflush(journal))
                    throw std::runtime_error("Reed journal flush failed");
                fclose(journal);
                journal = nullptr;
            } else
                throw std::runtime_error("Could not journal reed placements");
            moves = valid;
            for (auto &move : moves) {
                ++applied;
                if (!write(move.first.node + FLORA_POSITION, move.destination) ||
                    !write(move.second.node + FLORA_POSITION, move.destination))
                    throw std::runtime_error("Reed placement readback failed");
            }
        } catch (...) {
            if (journal)
                fclose(journal);
            // Restore original positions of the moves already attempted.
            for (size_t i = applied; i > 0; i--) {
                Bytes position(moves[i - 1].first.before.begin() + FLORA_POSITION,
                               moves[i - 1].first.before.begin() + FLORA_POSITION +
                                   FLORA_POSITION_SIZE);
                write(moves[i - 1].first.node + FLORA_POSITION, position);
                write(moves[i - 1].second.node + FLORA_POSITION, position);
            }
            resume(process);
            throw;
        }
        resume(process);
        return applied;
    }

    // Moves up to MAX_REED_MOVES reed clumps standing in water onto dry grass banks.
    // Runs every 3 s (500 ms while a backlog remains) or when new chunks load.
    void relocateReeds() {
        ULONGLONG now = GetTickCount64();
        if (now - lastReeds < (reedBacklog ? 500 : 3000) && !loaded.changed)
            return;
        lastReeds = now;
        int px, py, pz;
        if (!playerPosition(px, py, pz))
            return;
        ChunkCache chunks = chunkCache();
        if (!chunks.valid())
            return;
        FloraScan scan = scanFlora(chunks, px, py);
        unsigned examined = 0;
        std::vector<Move> moves;
        std::map<std::pair<int, int>, ShoreCell> cells;
        auto cell = [&](int x, int y) {
            auto key = std::make_pair(x, y);
            auto found = cells.find(key);
            if (found == cells.end())
                found = cells.emplace(key, shoreCell(x, y)).first;
            found->second.rock = scan.rockLocations.count(key) != 0;
            return found->second;
        };
        // A site is taken if something stands on it or a reed centre is closer
        // than 1.1 blocks. With integer cells that means the cell itself or one of
        // its four direct neighbours (distance 1).
        auto occupied = [&](int x, int y) {
            if (scan.reserved.count({x, y}))
                return true;
            static const int SELF_AND_NEIGHBOURS[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (const auto &d : SELF_AND_NEIGHBOURS)
                if (scan.reedLocations.count({x + d[0], y + d[1]}))
                    return true;
            return false;
        };
        for (auto &group : scan.groups) {
            int cx = group.cx, cy = group.cy;
            for (auto &entry : group.first) {
                if (moves.size() >= MAX_REED_MOVES)
                    break;
                // Only clumps listed exactly once in each list are moved.
                auto second = group.second.find(entry.first);
                if (entry.second.size() != 1 || second == group.second.end() ||
                    second->second.size() != 1)
                    continue;
                auto &first = entry.second[0];
                auto &other = second->second[0];
                int x = blockFromFixed(entry.first[0]), y = blockFromFixed(entry.first[1]);
                if (std::abs(x - px) > SCAN_RADIUS || std::abs(y - py) > SCAN_RADIUS)
                    continue;
                ++examined;
                int nx, ny, nz;
                auto source = cell(x, y);
                if (source.depth == 0 && dryBank(source))
                    continue; // Already on a dry bank.
                // Treat the clump's own cell as water at the clump's height.
                auto placementCell = [&](int xx, int yy) {
                    auto c = cell(xx, yy);
                    if (xx == x && yy == y && c.depth == 0) {
                        c.depth = 1;
                        c.water = blockFromFixed(entry.first[2]) - 1;
                    }
                    return c;
                };
                if (!chooseReedBank(x, y, cx, cy, placementCell, occupied, nx, ny, nz))
                    continue;
                Bytes destination(FLORA_POSITION_SIZE);
                int64_t coords[3] = {(int64_t)nx * 65536 + 32768, (int64_t)ny * 65536 + 32768,
                                     (int64_t)nz * 65536};
                memcpy(destination.data(), coords, FLORA_POSITION_SIZE);
                moves.push_back({first, other, destination});
                scan.reserved.insert({nx, ny});
                scan.reedLocations.insert({nx, ny});
            }
        }
        reedBacklog = moves.size() == MAX_REED_MOVES;
        if (moves.empty()) {
            if (examined && now - lastReedLog >= 15000) {
                char message[128];
                sprintf_s(
                    message,
                    "Shore scan: %u paired clumps, no unoccupied dry bank within shoreline patches",
                    examined);
                log(message);
                lastReedLog = now;
            }
            return;
        }
        size_t applied = applyReedMoves(moves);
        if (applied) {
            char message[128];
            sprintf_s(message,
                      "Moved %u reed clumps ashore; native stems only, terrain/rocks unchanged",
                      (unsigned)applied);
            log(message);
        }
    }

    // ---- Water conversion ------------------------------------------------------

    // Records which chunks are loaded near the player and whether they changed
    // since the last pass (new chunk or regenerated terrain/flora).
    void observeLoadedChunks() {
        int x, y, z;
        if (!playerPosition(x, y, z))
            return;
        ChunkCache chunks = chunkCache();
        if (!chunks.valid())
            return;
        loaded.begin();
        for (int cy = std::max(0, y - SCAN_RADIUS) / CHUNK_SIZE;
             cy <= (y + SCAN_RADIUS) / CHUNK_SIZE; ++cy)
            for (int cx = std::max(0, x - SCAN_RADIUS) / CHUNK_SIZE;
                 cx <= (x + SCAN_RADIUS) / CHUNK_SIZE; ++cx) {
                DWORD chunk = loadedChunk(chunks, cx, cy);
                if (!chunk)
                    continue;
                DWORD cols = columns(cx * CHUNK_SIZE, cy * CHUNK_SIZE);
                if (!cols)
                    continue;
                // Flora sentinel replacement also identifies native chunk regeneration.
                auto flora = read(chunk + CHUNK_FLORA_LISTS, 8);
                if (flora.size() != 8)
                    continue;
                uint64_t identity = (uint64_t(cols) << 32) |
                                    (value<DWORD>(flora, 0) ^ (value<DWORD>(flora, 4) * 16777619u));
                loaded.observe(cx, cy, identity);
            }
        loaded.end();
    }

    // Works out the new cells for one column, or returns false if it stays as is.
    // `height` receives the water surface and `newBase` the column's new base Z.
    bool planColumn(int nx, int ny, int bz, const Bytes &cells, Bytes &next, int &height,
                    int &newBase) {
        DWORD count = (DWORD)cells.size() / 4;
        bool hasSolid = false;
        int highest = -1;
        for (DWORD i = 0; i < count; ++i)
            if (material(cells, i) == MATERIAL_SOLID_WATER) {
                hasSolid = true;
                highest = i;
            }
        bool plain = plainWater(cells);
        height = bz;
        newBase = bz;
        if (hasSolid && !plain) {
            // Mixed column: only the water cells change.
            next = liquidize(cells);
            height = bz + highest;
        } else {
            Bytes original = cells;
            if (!plain) {
                // A column converted earlier (by us): rebuild the plain-water
                // original so its depth can be recomputed for its neighbours.
                auto known = converted.find({nx, ny});
                if (known == converted.end())
                    return false;
                height = known->second;
                int wet = 0;
                while (wet < (int)count && material(cells, wet) == MATERIAL_LIQUID_WATER)
                    ++wet;
                bool valid = wet > 0 && bz + wet - 1 == height;
                for (int i = wet; i < (int)count; ++i)
                    if (material(cells, i))
                        valid = false;
                if (!valid)
                    return false;
                std::array<unsigned char, 4> air{{255, 255, 255, 0}};
                if (wet < (int)count)
                    memcpy(air.data(), cells.data() + wet * 4, 4);
                original[3] = (cells[3] & MATERIAL_FLAGS) | MATERIAL_SOLID_WATER;
                for (DWORD i = 1; i < count; ++i)
                    memcpy(original.data() + i * 4, air.data(), 4);
            }
            // Plain water: deepen downwards from the surface.
            int d = std::min(depth(nx, ny, height), (int)count);
            next = deepen(original, d);
            newBase = height - (d - 1);
        }
        return !(next.empty() || (next == cells && newBase == bz));
    }

    // Validates, journals and writes a batch of column changes while the game is
    // suspended, then flags the affected meshes for rebuild. Returns the changes
    // that were applied. Throws on failure after rolling back.
    std::vector<Change> applyColumnChanges(const std::vector<Change> &changes) {
        if (suspend(process) != 0)
            throw std::runtime_error("Could not pause terrain safely");
        std::vector<Change> valid;
        size_t done = 0;
        try {
            for (auto &c : changes)
                if (ptr(c.column + COLUMN_CELLS) == c.data &&
                    ptr(c.column + COLUMN_COUNT) == c.count &&
                    read(c.column + COLUMN_BASE_Z, 4) == intBytes(c.oldBase) &&
                    read(c.data, c.count * 4) == c.before && flags(c.x, c.y) == c.flags)
                    valid.push_back(c);
            if (!valid.empty() && !journal(valid))
                throw std::runtime_error("Could not journal terrain originals");
            for (auto &c : valid) {
                ++done;
                if (!write(c.data, c.after) ||
                    !write(c.column + COLUMN_BASE_Z, intBytes(c.newBase)))
                    throw std::runtime_error("Terrain write/readback failed");
            }
            for (auto &c : valid)
                for (DWORD f : c.flags)
                    if (!write(f, Bytes{1}))
                        throw std::runtime_error("Mesh update failed");
        } catch (...) {
            for (size_t i = done; i > 0; --i) {
                auto &c = valid[i - 1];
                write(c.data, c.before);
                write(c.column + COLUMN_BASE_Z, intBytes(c.oldBase));
            }
            resume(process);
            throw;
        }
        resume(process);
        return valid;
    }

    // Converts up to MAX_COLUMN_CHANGES water columns of the nearest pending
    // loaded chunk. Returns the number of columns changed.
    int step() {
        int x, y, z;
        if (!playerPosition(x, y, z)) {
            ULONGLONG now = GetTickCount64();
            if (now - lastFind < 3000)
                return 0;
            lastFind = now;
            find();
            if (!playerPosition(x, y, z))
                return 0;
        }
        std::vector<Change> changes;
        int cx = 0, cy = 0;
        auto *work = loaded.next(x / CHUNK_SIZE, y / CHUNK_SIZE, GetTickCount64(), cx, cy);
        if (!work)
            return 0;
        DWORD cols = columns(cx * CHUNK_SIZE, cy * CHUNK_SIZE);
        if (!cols)
            return 0;
        // Read only this loaded tile's 32 header rows (32 KiB, not a 2 MiB zone).
        Bytes block(CHUNK_SIZE * CHUNK_SIZE * COLUMN_HEADER_SIZE);
        for (int row = 0; row < CHUNK_SIZE; ++row) {
            DWORD offset = ((cx * CHUNK_SIZE % 256) + (cy * CHUNK_SIZE % 256 + row) * 256) *
                           COLUMN_HEADER_SIZE;
            auto headers = read(cols + offset, CHUNK_SIZE * COLUMN_HEADER_SIZE);
            if (headers.size() != CHUNK_SIZE * COLUMN_HEADER_SIZE)
                return 0;
            memcpy(block.data() + row * CHUNK_SIZE * COLUMN_HEADER_SIZE, headers.data(),
                   headers.size());
        }
        // Resume where the previous batch for this chunk stopped.
        for (; work->cursor < 1024 && changes.size() < MAX_COLUMN_CHANGES; ++work->cursor) {
            int nx = cx * CHUNK_SIZE + work->cursor % CHUNK_SIZE;
            int ny = cy * CHUNK_SIZE + work->cursor / CHUNK_SIZE;
            size_t offset = work->cursor * COLUMN_HEADER_SIZE;
            DWORD count = value<DWORD>(block, offset + COLUMN_COUNT),
                  data = value<DWORD>(block, offset + COLUMN_CELLS);
            int bz = value<int>(block, offset + COLUMN_BASE_Z);
            if (count < 1 || count > MAX_COLUMN_CELLS || bz <= -10000 || bz >= 10000)
                continue;
            Bytes cells = read(data, count * 4);
            if (cells.size() != count * 4)
                continue;
            Bytes next;
            int height, newBase;
            if (!planColumn(nx, ny, bz, cells, next, height, newBase))
                continue;
            auto dirty = flags(nx, ny);
            if (dirty.empty())
                continue;
            changes.push_back({nx, ny, height, bz, newBase, cols + columnOffset(nx, ny), data,
                               count, cells, next, dirty});
        }
        if (work->cursor == 1024) {
            work->cursor = 0;
            work->completed = GetTickCount64();
        }
        if (changes.empty())
            return 0;
        std::vector<Change> valid = applyColumnChanges(changes);
        for (auto &c : valid)
            converted[{c.x, c.y}] = c.surface;
        if (!valid.empty()) {
            char b[100];
            sprintf_s(b, "Converted %u columns; stones preserved", (unsigned)valid.size());
            log(b);
        }
        return (int)valid.size();
    }
};

// ===========================================================================
// Exports and startup
// ===========================================================================

// Returns 0 when the column transformations behave as expected.
extern "C" __declspec(dllexport) int __cdecl WaterSelfTest() {
    Bytes simple = {20, 40, 60, 0xa3, 255, 255, 255, 0, 255, 255, 255, 0};
    auto d = deepen(simple, 3);
    if (d.size() != 12 || d[3] != 0xa2 || d[7] != 0xa2 || d[11] != 0xa2)
        return 1;
    auto slope = deepen(simple, 2);
    if (slope[11] != 0 || slope[7] != 0xa2)
        return 2;
    Bytes single = {1, 2, 3, 3};
    if (deepen(single, 3) != Bytes({1, 2, 3, 2}))
        return 3;
    Bytes mixed = {1, 2, 3, 3, 7, 8, 9, 1, 4, 5, 6, 0xe3};
    auto m = liquidize(mixed);
    if (m[3] != 2 || m[11] != 0xe2 || memcmp(m.data() + 4, mixed.data() + 4, 4) ||
        !deepen(mixed, 3).empty())
        return 4;
    return ReedSelfTest() * 100;
}

// rundll32 entry point. `arguments` is the game's process id.
extern "C" __declspec(dllexport) void CALLBACK WaterWorker(HWND, HINSTANCE, LPSTR arguments, int) {
    DWORD pid = strtoul(arguments, nullptr, 10);
    if (!pid)
        return;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\CubeAlphaAutomaticWater");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return;
    }
    try {
        Patch patch;
        if (!patch.init(pid)) {
            log("Water worker could not attach to supported build");
            CloseHandle(mutex);
            return;
        }
        log("Native water + shore reeds worker active: 2-3 depth, stones preserved, no added root geometry");
        DWORD code = 0;
        while (GetExitCodeProcess(patch.process, &code) && code == STILL_ACTIVE) {
            patch.observeLoadedChunks();
            patch.step();
            patch.relocateReeds();
            patch.repairReedOwners();
            Sleep(50);
        }
        log("Water worker stopped with game");
    } catch (const std::exception &e) {
        log(e.what());
    } catch (...) {
        log("Water worker stopped after unexpected failure");
    }
    CloseHandle(mutex);
}

// Launches the hidden worker: rundll32 "<this dll>",WaterWorker <pid>.
static DWORD WINAPI startWorker(LPVOID) {
    InitializeAlphaReeds(ownModule);
    wchar_t system[32768];
    GetSystemDirectoryW(system, 32768);
    std::wstring exe = std::wstring(system) + L"\\rundll32.exe";
    std::wstring command = L"\"" + exe + L"\" \"" + dllPath() + L"\",WaterWorker " +
                           std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION p{};
    if (CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, root().c_str(), &startup, &p)) {
        CloseHandle(p.hThread);
        CloseHandle(p.hProcess);
    } else
        log("Could not start native water worker");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ownModule = module;
        wchar_t path[32768];
        GetModuleFileNameW(nullptr, path, 32768);
        const wchar_t *name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        if (!_wcsicmp(name, L"Cube.exe") || !_wcsicmp(name, L"Cube-SteamWater-Preview.exe")) {
            HANDLE thread = CreateThread(nullptr, 0, startWorker, nullptr, 0, nullptr);
            if (thread)
                CloseHandle(thread);
        }
    }
    return TRUE;
}
