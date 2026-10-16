#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
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
static std::wstring root() {
    return parent(parent(dllPath()));
}
static void log(const char *text) {
    FILE *f = nullptr;
    _wfopen_s(&f, (root() + L"\\water-port\\native-water.log").c_str(), L"a");
    if (f) {
        fprintf(f, "%llu %s\n", GetTickCount64(), text);
        fclose(f);
    }
}
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
static bool plainWater(const Bytes &b) {
    if (b.empty() || b.size() % 4 || (b[3] & 31) != 3)
        return false;
    for (size_t i = 7; i < b.size(); i += 4)
        if (b[i] & 31)
            return false;
    return true;
}
static Bytes deepen(const Bytes &b, int depth) {
    if (!plainWater(b))
        return {};
    int count = (int)b.size() / 4;
    depth = std::min(depth, count);
    Bytes out = b;
    for (int i = 0; i < count; ++i) {
        if (i < depth) {
            memcpy(out.data() + i * 4, b.data(), 4);
            out[i * 4 + 3] = (b[3] & 224) | 2;
        } else
            memcpy(out.data() + i * 4, b.data() + 4, 4);
    }
    return out;
}
static Bytes liquidize(Bytes b) {
    for (size_t i = 3; i < b.size(); i += 4)
        if ((b[i] & 31) == 3)
            b[i] = (b[i] & 224) | 2;
    return b;
}
template <class T> static T value(const Bytes &b, size_t offset) {
    T v{};
    if (offset + sizeof(T) <= b.size())
        memcpy(&v, b.data() + offset, sizeof(T));
    return v;
}
struct Change {
    int x, y, surface, oldBase, newBase;
    DWORD column, data, count;
    Bytes before, after;
    std::vector<DWORD> flags;
};
struct Patch {
    HANDLE process{};
    DWORD pid{}, base{}, dataRva{}, dataSize{}, vtable{}, game{}, layout{};
    ULONGLONG lastFind{}, lastReeds{}, lastReedLog{}, lastOwnerRepair{};
    LoadedChunkWork loaded;
    bool reedBacklog = true;
    std::map<std::pair<int, int>, int> converted;
    // Native chunk cache headers, refreshed with one read per pass.
    Bytes cacheSlots;
    DWORD cacheAddress{}, cacheWidth{};
    // Zone column tables, looked up once per pass instead of per column.
    std::map<std::pair<int, int>, DWORD> zoneColumns;
    // Shoreline cells of loaded chunks, read in bulk and kept until the chunk
    // unloads or regenerates. Water conversion only deepens water, so the
    // dry-bank and water-depth tests these feed are unaffected by it.
    struct ShoreTile {
        uint64_t identity{};
        std::vector<ShoreCell> cells;
    };
    std::map<std::pair<int, int>, ShoreTile> shoreTiles;
    // Chunks whose clumps moved this pass: their native list ownership is
    // repaired right away instead of on the quiet cadence.
    std::set<std::pair<int, int>> movedFrom;
    using NtCall = LONG(NTAPI *)(HANDLE);
    NtCall suspend{}, resume{};
    ~Patch() {
        if (process)
            CloseHandle(process);
    }
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
        process = OpenProcess(0xC38, FALSE, pid);
        if (!process)
            return false;
        auto ntdll = GetModuleHandleW(L"ntdll.dll");
        suspend = (NtCall)GetProcAddress(ntdll, "NtSuspendProcess");
        resume = (NtCall)GetProcAddress(ntdll, "NtResumeProcess");
        if (!suspend || !resume)
            return false;
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
    bool player(DWORD g, int &x, int &y, int &z) {
        DWORD p = ptr(g + layout + 0x8006c8);
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
    DWORD columns(int x, int y) {
        if (x < 0 || y < 0 || x >= 16777216 || y >= 16777216)
            return 0;
        int zx = x / 256, zy = y / 256;
        auto known = zoneColumns.find({zx, zy});
        if (known != zoneColumns.end())
            return known->second;
        DWORD region = ptr(game + layout + 0x398 + ((zx / 64) * 1024 + zy / 64) * 4);
        DWORD zone = region ? ptr(region + 0x10018 + ((zx % 64) * 64 + zy % 64) * 4) : 0;
        Bytes xy = read(zone + 0x60, 8);
        DWORD table = 0;
        if (zone && xy.size() == 8 && value<int>(xy, 0) == zx && value<int>(xy, 4) == zy)
            table = ptr(zone + 0xa8);
        zoneColumns[{zx, zy}] = table;
        return table;
    }
    // Loaded chunk (cx, cy) in this pass's cache snapshot, or 0.
    DWORD chunkAt(int cx, int cy) {
        if (cx < 0 || cy < 0 || !cacheWidth || cacheSlots.empty())
            return 0;
        size_t slot = ((cy % cacheWidth) * cacheWidth + cx % cacheWidth) * 0x268;
        return value<int>(cacheSlots, slot + 0x18) == cx &&
                       value<int>(cacheSlots, slot + 0x1c) == cy
                   ? cacheAddress + (DWORD)slot
                   : 0;
    }
    // Mesh dirty flags of the chunks around a column. `live` re-reads the
    // native cache (used to validate a batch while the game is paused).
    std::vector<DWORD> flags(int x, int y, bool live = false) {
        DWORD width = cacheWidth, cache = cacheAddress;
        if (live) {
            width = ptr(game + layout + 0x2d4);
            cache = ptr(game + layout + 0x2d8);
        }
        std::vector<DWORD> out;
        if (!cache || width < 1 || width > 64)
            return out;
        for (int cx = (x - 1) / 32; cx <= (x + 1) / 32; ++cx)
            for (int cy = (y - 1) / 32; cy <= (y + 1) / 32; ++cy) {
                DWORD chunk = 0;
                if (!live)
                    chunk = chunkAt(cx, cy);
                else {
                    DWORD slot = cache + ((cy % width) * width + cx % width) * 0x268;
                    Bytes xy = read(slot + 0x18, 8);
                    if (xy.size() == 8 && value<int>(xy, 0) == cx && value<int>(xy, 4) == cy)
                        chunk = slot;
                }
                if (chunk)
                    out.push_back(chunk + 0x74);
            }
        return out;
    }
    bool surface(int x, int y, int &height) {
        DWORD c = columns(x, y);
        if (!c)
            return false;
        Bytes col = read(c + (x % 256 + y % 256 * 256) * 32, 32);
        DWORD count = value<DWORD>(col, 28);
        if (count < 1 || count > 2048)
            return false;
        Bytes cells = read(value<DWORD>(col, 24), count * 4);
        if (cells.size() != count * 4)
            return false;
        int bz = value<int>(col, 16);
        if (plainWater(cells)) {
            height = bz;
            return true;
        }
        int top = -1;
        for (DWORD i = 0; i < count; ++i) {
            int t = cells[i * 4 + 3] & 31;
            if (t != 0 && t != 2)
                return false;
            if (t == 2)
                top = i;
        }
        if (top < 0)
            return false;
        height = bz + top;
        return true;
    }
    int depth(int x, int y, int height) {
        int h;
        for (auto d : std::array<std::pair<int, int>, 4>{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}})
            if (surface(x + d.first, y + d.second, h) && h != height)
                return 2;
        return 3;
    }
    bool journal(const std::vector<Change> &batch) {
        FILE *f = nullptr;
        _wfopen_s(&f, (root() + L"\\water-port\\native-water-originals.bin").c_str(), L"ab");
        if (!f)
            return false;
        DWORD header[] = {0x41575031, pid, (DWORD)batch.size()};
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
    ShoreCell shoreCell(int x, int y) {
        DWORD cols = columns(x, y);
        if (!cols)
            return {};
        return shoreColumn(read(cols + (x % 256 + y % 256 * 256) * 32, 32));
    }
    // Shore cell at (x, y): loaded chunks come from a cached bulk read.
    ShoreCell shoreAt(int x, int y) {
        if (x < 0 || y < 0)
            return {};
        std::pair<int, int> tile{x / 32, y / 32};
        auto state = loaded.chunks.find(tile);
        if (state == loaded.chunks.end())
            return shoreCell(x, y);
        auto &cached = shoreTiles[tile];
        if (cached.cells.empty() || cached.identity != state->second.identity) {
            cached.identity = state->second.identity;
            cached.cells.assign(1024, ShoreCell{});
            DWORD cols = columns(tile.first * 32, tile.second * 32);
            bool complete = cols != 0;
            for (int row = 0; complete && row < 32; ++row) {
                DWORD offset =
                    ((tile.first * 32 % 256) + (tile.second * 32 % 256 + row) * 256) * 32;
                Bytes headers = read(cols + offset, 32 * 32);
                complete = headers.size() == 32 * 32;
                for (int column = 0; complete && column < 32; ++column)
                    cached.cells[row * 32 + column] = shoreColumn(
                        Bytes(headers.begin() + column * 32, headers.begin() + column * 32 + 32));
            }
            if (!complete) { // Retry the bulk read on the next request.
                cached.cells.clear();
                return shoreCell(x, y);
            }
        }
        return cached.cells[(y % 32) * 32 + x % 32];
    }
    ShoreCell shoreColumn(const Bytes &column) {
        ShoreCell out;
        DWORD count = value<DWORD>(column, 28);
        int bz = value<int>(column, 16);
        if (column.size() != 32 || count < 1 || count > 2048)
            return out;
        Bytes cells = read(value<DWORD>(column, 24), count * 4);
        if (cells.size() != count * 4)
            return out;
        out.valid = true;
        out.bottom = bz;
        int highestWater = -1;
        for (DWORD i = 0; i < count; i++) {
            int type = cells[i * 4 + 3] & 31;
            if (type == 2 || type == 3) {
                highestWater = (int)i;
                out.water = bz + i;
            } else if (type) {
                out.ground = bz + i;
                out.groundType = type;
            }
        }
        if (highestWater >= 0) {
            for (int i = highestWater; i >= 0; i--) {
                int type = cells[i * 4 + 3] & 31;
                if (type != 2 && type != 3)
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
    void repairReedOwners() {
        auto now = GetTickCount64();
        // Chunks whose clumps just moved are repaired at once; the quiet
        // cadence still checks everything near the player.
        std::set<std::pair<int, int>> sources;
        sources.swap(movedFrom);
        bool quiet = now - lastOwnerRepair >= 3000;
        if (sources.empty() && !quiet)
            return;
        int px, py, pz;
        if (!game || !player(game, px, py, pz))
            return;
        if (quiet) {
            lastOwnerRepair = now;
            for (int cy = (py - 96) / 32; cy <= (py + 96) / 32; cy++)
                for (int cx = (px - 96) / 32; cx <= (px + 96) / 32; cx++)
                    sources.insert({cx, cy});
        }
        unsigned transferred = 0;
        for (auto &chunk : sources) {
            int cx = chunk.first, cy = chunk.second;
            DWORD source = chunkAt(cx, cy);
            if (!source)
                continue;
            DWORD head = ptr(source + 0x248), node = ptr(head);
            std::set<DWORD> seen;
            while (node && node != head && seen.size() < 4096) {
                if (!seen.insert(node).second)
                    break;
                Bytes record = read(node, 72);
                if (record.size() != 72)
                    break;
                DWORD next = value<DWORD>(record, 0);
                int x = (int)(value<int64_t>(record, 16) / 65536),
                    y = (int)(value<int64_t>(record, 24) / 65536);
                if (value<DWORD>(record, 8) != 22 || x / 32 == cx && y / 32 == cy) {
                    node = next;
                    continue;
                }
                DWORD destination = chunkAt(x / 32, y / 32);
                if (!destination) {
                    node = next;
                    continue;
                }
                if (suspend(process) != 0)
                    return;
                std::map<DWORD, std::pair<Bytes, Bytes>> edits;
                size_t applied = 0;
                try {
                    if (read(node, 72) != record || ptr(source + 0x248) != head || !destination)
                        throw std::runtime_error("Reed ownership changed before repair");
                    DWORD nodes[2]{};
                    for (int copy = 0; copy < 2; copy++) {
                        DWORD h = ptr(source + 0x240 + copy * 8), n = ptr(h);
                        unsigned count = 0, matches = 0;
                        std::set<DWORD> visited;
                        while (n && n != h && visited.size() < 4096) {
                            if (!visited.insert(n).second)
                                break;
                            Bytes b = read(n, 72);
                            if (b.size() != 72)
                                break;
                            if (value<DWORD>(b, 8) == 22 &&
                                !memcmp(b.data() + 16, record.data() + 16, 24)) {
                                nodes[copy] = n;
                                matches++;
                            }
                            n = value<DWORD>(b, 0);
                            count++;
                        }
                        if (n != h || matches != 1 || count != ptr(source + 0x244 + copy * 8))
                            throw std::runtime_error("Native foliage list incomplete");
                    }
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
                        DWORD sh = ptr(source + 0x240 + copy * 8),
                              dh = ptr(destination + 0x240 + copy * 8);
                        transferReedLinks(
                            nodes[copy], sh, dh, source + 0x244 + copy * 8,
                            destination + 0x244 + copy * 8, [&](DWORD a) { return ptr(a); },
                            [&](DWORD a, DWORD v) { stage(a, intBytes(v)); });
                    }
                    // Preserve native vegetation: full terrain rebuild regenerates its original placements.
                    FILE *journal = nullptr;
                    if (_wfopen_s(&journal,
                                  (root() + L"\\water-port\\reed-ownership-originals.bin").c_str(),
                                  L"ab") ||
                        !journal)
                        throw std::runtime_error("Ownership journal unavailable");
                    bool ok = true;
                    DWORD header[] = {0x41524f31, pid, (DWORD)edits.size()};
                    ok = fwrite(header, sizeof(header), 1, journal) == 1;
                    for (auto &e : edits) {
                        DWORD info[] = {e.first, (DWORD)e.second.first.size()};
                        ok =
                            ok && fwrite(info, sizeof(info), 1, journal) == 1 &&
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
                    ++transferred;
                } catch (const std::exception &e) {
                    size_t index = 0;
                    for (auto &edit : edits) {
                        if (index++ >= applied)
                            break;
                        write(edit.first, edit.second.first);
                    }
                    resume(process);
                    log(e.what());
                }
                // `next` was read before the transfer and is still in this list.
                node = next;
            }
        }
        if (transferred) {
            char message[128];
            sprintf_s(message,
                      "Reed ownership of %u clumps transferred to bank chunks; native vegetation "
                      "retained without terrain regeneration",
                      transferred);
            log(message);
        }
    }
    // True when every chunk a bank search around (x, y) reads is loaded: the
    // 48-block search square plus its 4-block shore check.
    bool searchLoaded(int x, int y) {
        for (int cy = (y - 52) / 32; cy <= (y + 52) / 32; ++cy)
            for (int cx = (x - 52) / 32; cx <= (x + 52) / 32; ++cx)
                if (!loaded.chunks.count({cx, cy}))
                    return false;
        return true;
    }
    void relocateReeds() {
        ULONGLONG now = GetTickCount64();
        // Newly loaded chunks are placed on the same pass they appear; a backlog
        // keeps running every pass. Quiet areas near the player are rechecked
        // every three seconds, as before.
        bool quiet = now - lastReeds >= 3000;
        if (!loaded.changed && !reedBacklog && !quiet)
            return;
        int px, py, pz;
        if (!game || !player(game, px, py, pz))
            return;
        if (quiet) {
            lastReeds = now;
            for (auto &c : loaded.chunks)
                if (std::abs(c.first.first * 32 + 16 - px) <= 112 &&
                    std::abs(c.first.second * 32 + 16 - py) <= 112)
                    c.second.reedsSettled = false;
        }
        struct Plant {
            DWORD node, head, chunk;
            Bytes before;
            std::array<int64_t, 3> position;
        };
        struct Move {
            Plant first, second;
            Bytes destination;
            std::pair<int, int> from;
        };
        struct Group {
            int cx, cy;
            std::map<std::array<int64_t, 3>, std::vector<Plant>> first, second;
        };
        // Unsettled chunks, nearest first. Neighbors within two chunks are read
        // too: their rocks, clumps and occupied sites constrain bank choices.
        constexpr size_t maxTargets = 24, maxMoves = 64;
        std::vector<std::pair<int, int>> targets;
        for (auto &c : loaded.chunks)
            if (!c.second.reedsSettled)
                targets.push_back(c.first);
        int pcx = px / 32, pcy = py / 32;
        std::sort(targets.begin(), targets.end(), [&](auto &a, auto &b) {
            int da = (a.first - pcx) * (a.first - pcx) + (a.second - pcy) * (a.second - pcy);
            int db = (b.first - pcx) * (b.first - pcx) + (b.second - pcy) * (b.second - pcy);
            return da < db;
        });
        bool more = targets.size() > maxTargets;
        if (more)
            targets.resize(maxTargets);
        std::set<std::pair<int, int>> wanted(targets.begin(), targets.end()), scan;
        for (auto &t : targets)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    scan.insert({t.first + dx, t.second + dy});
        std::map<std::pair<int, int>, Group> groups;
        unsigned examined = 0, pending = 0;
        std::vector<Move> moves;
        std::set<std::pair<int, int>> reserved, rockLocations, reedLocations;
        auto cell = [&](int x, int y) {
            auto c = shoreAt(x, y);
            c.rock = rockLocations.count({x, y}) != 0;
            return c;
        };
        for (auto &key : scan) {
            int cx = key.first, cy = key.second;
            DWORD chunk = chunkAt(cx, cy);
            if (!chunk)
                continue;
            std::map<std::array<int64_t, 3>, std::vector<Plant>> lists[2];
            bool complete = true;
            for (int copy = 0; copy < 2; copy++) {
                DWORD head = ptr(chunk + 0x240 + copy * 8), node = ptr(head);
                std::set<DWORD> seen;
                while (node && node != head && seen.size() < 4096) {
                    if (!seen.insert(node).second) {
                        complete = false;
                        break;
                    }
                    Bytes b = read(node, 72);
                    if (b.size() != 72) {
                        complete = false;
                        break;
                    }
                    DWORD kind = value<DWORD>(b, 8);
                    std::array<int64_t, 3> position{value<int64_t>(b, 16), value<int64_t>(b, 24),
                                                    value<int64_t>(b, 32)};
                    int x = (int)(position[0] / 65536), y = (int)(position[1] / 65536);
                    if (kind == 22)
                        lists[copy][position].push_back({node, head, chunk, b, position});
                    if (kind == 1) {
                        for (int ry = -4; ry <= 4; ry++)
                            for (int rx = -4; rx <= 4; rx++)
                                rockLocations.insert({x + rx, y + ry});
                    }
                    if (kind == 22)
                        reedLocations.insert({x, y});
                    if (kind == 1 || kind == 22 || kind >= 31)
                        reserved.insert({x, y});
                    node = value<DWORD>(b, 0);
                }
                if (node != head)
                    complete = false;
            }
            if (complete && wanted.count(key))
                groups[key] = {cx, cy, std::move(lists[0]), std::move(lists[1])};
        }
        // Clump centers stay at least 1.1 blocks apart: only the site itself
        // and its four edge neighbors are closer than that.
        auto occupied = [&](int x, int y) {
            if (reserved.count({x, y}))
                return true;
            for (auto d :
                 std::array<std::pair<int, int>, 5>{{{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}}})
                if (reedLocations.count({x + d.first, y + d.second}))
                    return true;
            return false;
        };
        for (auto &key : targets) {
            auto found = groups.find(key);
            if (found == groups.end()) {
                more = true; // Lists were mid-update; retry next pass.
                continue;
            }
            auto &group = found->second;
            int cx = group.cx, cy = group.cy;
            bool capped = false;
            for (auto &entry : group.first) {
                if (moves.size() >= maxMoves) {
                    capped = true;
                    break;
                }
                // Both native list copies hold each clump; identical placements
                // at one position are paired in list order.
                auto second = group.second.find(entry.first);
                if (second == group.second.end() || second->second.size() != entry.second.size())
                    continue;
                int x = (int)(entry.first[0] / 65536), y = (int)(entry.first[1] / 65536);
                int z = (int)(entry.first[2] / 65536);
                auto source = cell(x, y);
                if (source.depth == 0 && dryBank(source))
                    continue;
                // Already sunk below the riverbed by an earlier pass.
                if (source.valid && z < source.bottom)
                    continue;
                for (size_t copy = 0; copy < entry.second.size() && moves.size() < maxMoves;
                     ++copy) {
                    auto &first = entry.second[copy];
                    auto &other = second->second[copy];
                    ++examined;
                    int nx, ny, nz;
                    auto placementCell = [&](int xx, int yy) {
                        auto c = cell(xx, yy);
                        if (xx == x && yy == y && c.depth == 0) {
                            c.depth = 1;
                            c.water = z - 1;
                        }
                        return c;
                    };
                    int64_t coords[3];
                    if (chooseReedBank(x, y, cx, cy, placementCell, occupied, nx, ny, nz)) {
                        coords[0] = (int64_t)nx * 65536 + 32768;
                        coords[1] = (int64_t)ny * 65536 + 32768;
                        coords[2] = (int64_t)nz * 65536;
                        reserved.insert({nx, ny});
                        reedLocations.insert({nx, ny});
                    } else if (source.valid && source.depth > 0 && searchLoaded(x, y)) {
                        // Standing in water with no free bank anywhere in the fully
                        // loaded search area: sink it three blocks under the
                        // riverbed (inside solid terrain), so it is hidden. Only
                        // its height changes; the clump itself is kept.
                        coords[0] = entry.first[0];
                        coords[1] = entry.first[1];
                        coords[2] = (int64_t)(source.bottom - 3) * 65536;
                    } else {
                        // Bank cells not loaded yet: retried when a neighbor loads.
                        ++pending;
                        continue;
                    }
                    Bytes destination(24);
                    memcpy(destination.data(), coords, 24);
                    moves.push_back({first, other, destination, key});
                }
                if (moves.size() >= maxMoves)
                    capped = true;
            }
            // Every clump here is now ashore or has no free site; it is revisited
            // when this chunk or a neighbor loads, or on the quiet recheck.
            if (capped)
                more = true;
            else
                loaded.chunks[key].reedsSettled = true;
        }
        reedBacklog = more;
        if (moves.empty()) {
            if (pending && now - lastReedLog >= 15000) {
                char message[160];
                sprintf_s(message,
                          "Shore scan: %u clumps in water wait for bank chunks to load (%u "
                          "examined)",
                          pending, examined);
                log(message);
                lastReedLog = now;
            }
            return;
        }
        if (suspend(process) != 0)
            throw std::runtime_error("Could not pause reed placements safely");
        size_t applied = 0;
        FILE *journal = nullptr;
        try {
            std::vector<Move> valid;
            for (auto &move : moves) {
                auto matches = [&](const Plant &p) {
                    return read(p.node, 72) == p.before &&
                           ptr(p.chunk + 0x240 + (p.head == ptr(p.chunk + 0x248) ? 8 : 0)) ==
                               p.head &&
                           ptr(value<DWORD>(p.before, 0) + 4) == p.node &&
                           ptr(value<DWORD>(p.before, 4)) == p.node;
                };
                if (matches(move.first) && matches(move.second))
                    valid.push_back(move);
            }
            if (!_wfopen_s(&journal,
                           (root() + L"\\water-port\\reed-placement-originals.bin").c_str(),
                           L"ab") &&
                journal) {
                DWORD header[] = {0x41525031, pid, (DWORD)valid.size()};
                if (fwrite(header, sizeof(header), 1, journal) != 1)
                    throw std::runtime_error("Reed journal failed");
                for (auto &move : valid) {
                    DWORD addresses[] = {move.first.node, move.second.node};
                    if (fwrite(addresses, sizeof(addresses), 1, journal) != 1 ||
                        fwrite(move.first.before.data() + 16, 24, 1, journal) != 1 ||
                        fwrite(move.destination.data(), 24, 1, journal) != 1)
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
                if (!write(move.first.node + 16, move.destination) ||
                    !write(move.second.node + 16, move.destination))
                    throw std::runtime_error("Reed placement readback failed");
            }
        } catch (...) {
            if (journal)
                fclose(journal);
            for (size_t i = applied; i > 0; i--) {
                Bytes position(moves[i - 1].first.before.begin() + 16,
                               moves[i - 1].first.before.begin() + 40);
                write(moves[i - 1].first.node + 16, position);
                write(moves[i - 1].second.node + 16, position);
            }
            resume(process);
            throw;
        }
        resume(process);
        for (auto &move : moves) {
            int64_t coords[2];
            memcpy(coords, move.destination.data(), 16);
            if (std::make_pair(int(coords[0] / 65536) / 32, int(coords[1] / 65536) / 32) !=
                move.from)
                movedFrom.insert(move.from);
        }
        if (applied) {
            unsigned hidden = 0;
            for (auto &move : moves)
                hidden += !memcmp(move.destination.data(), move.first.before.data() + 16, 16);
            char message[160];
            sprintf_s(message,
                      "Moved %u reed clumps ashore, sank %u with no free bank under the "
                      "riverbed; native stems only, terrain/rocks unchanged",
                      (unsigned)applied - hidden, hidden);
            log(message);
        }
    }
    // Locates the active character; retried every second while it is absent.
    bool ready(int &x, int &y, int &z) {
        if (game && player(game, x, y, z))
            return true;
        ULONGLONG now = GetTickCount64();
        if (now - lastFind < 1000)
            return false;
        lastFind = now;
        find();
        return game && player(game, x, y, z);
    }
    void observeLoadedChunks() {
        zoneColumns.clear();
        int x, y, z;
        if (!ready(x, y, z))
            return;
        cacheWidth = ptr(game + layout + 0x2d4);
        cacheAddress = ptr(game + layout + 0x2d8);
        cacheSlots.clear();
        if (cacheAddress && cacheWidth >= 1 && cacheWidth <= 64)
            cacheSlots = read(cacheAddress, cacheWidth * cacheWidth * 0x268);
        if (cacheSlots.empty()) {
            cacheWidth = 0;
            return;
        }
        loaded.begin();
        // Every chunk the game holds (its cache is a ring around the player),
        // not only those near the character, so water and reeds are already
        // done when the player arrives.
        int half = (int)cacheWidth / 2, pcx = x / 32, pcy = y / 32;
        for (int cy = std::max(0, pcy - half); cy <= pcy + half; ++cy)
            for (int cx = std::max(0, pcx - half); cx <= pcx + half; ++cx) {
                DWORD chunk = chunkAt(cx, cy);
                if (!chunk)
                    continue;
                DWORD cols = columns(cx * 32, cy * 32);
                if (!cols)
                    continue;
                // Flora sentinel replacement also identifies native chunk regeneration.
                size_t slot = chunk - cacheAddress;
                uint64_t identity =
                    (uint64_t(cols) << 32) | (value<DWORD>(cacheSlots, slot + 0x240) ^
                                              (value<DWORD>(cacheSlots, slot + 0x244) * 16777619u));
                loaded.observe(cx, cy, identity);
            }
        loaded.end();
        for (auto it = shoreTiles.begin(); it != shoreTiles.end();) {
            auto state = loaded.chunks.find(it->first);
            if (state == loaded.chunks.end() || state->second.identity != it->second.identity)
                it = shoreTiles.erase(it);
            else
                ++it;
        }
    }
    // Converts water in loaded chunks, nearest first, until `budgetMs` has
    // passed. Returns true when it stopped with work possibly remaining.
    bool convertWater(double budgetMs) {
        int x, y, z;
        if (!game || cacheSlots.empty() || !player(game, x, y, z))
            return false;
        LARGE_INTEGER frequency, start, now;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        for (;;) {
            int cx = 0, cy = 0;
            ULONGLONG tick = GetTickCount64();
            auto *work = loaded.next(x / 32, y / 32, tick, cx, cy, 3);
            if (!work)
                return false;
            if (convertTile(*work, cx, cy) < 0)
                work->retry = tick + 250;
            QueryPerformanceCounter(&now);
            if ((now.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart >= budgetMs)
                return true;
        }
    }
    // Converts up to one batch of a tile, resuming at its cursor. Returns the
    // number of columns written, or -1 when the tile could not be read.
    int convertTile(LoadedChunkWork::State &work, int cx, int cy) {
        std::vector<Change> changes;
        DWORD cols = columns(cx * 32, cy * 32);
        if (!cols)
            return -1;
        // Read only this loaded tile's 32 header rows (32 KiB, not a 2 MiB zone).
        Bytes block(32 * 32 * 32);
        for (int row = 0; row < 32; ++row) {
            DWORD offset = ((cx * 32 % 256) + (cy * 32 % 256 + row) * 256) * 32;
            auto headers = read(cols + offset, 32 * 32);
            if (headers.size() != 32 * 32)
                return -1;
            memcpy(block.data() + row * 32 * 32, headers.data(), headers.size());
        }
        // Up to 256 columns per pause keeps each pause to a few milliseconds.
        for (; work.cursor < 1024 && changes.size() < 256; ++work.cursor) {
            int nx = cx * 32 + work.cursor % 32;
            int ny = cy * 32 + work.cursor / 32;
            size_t offset = work.cursor * 32;
            DWORD count = value<DWORD>(block, offset + 28), data = value<DWORD>(block, offset + 24);
            int bz = value<int>(block, offset + 16);
            if (count < 1 || count > 2048 || bz <= -10000 || bz >= 10000)
                continue;
            Bytes cells = read(data, count * 4);
            if (cells.size() != count * 4)
                continue;
            bool hasSolid = false;
            int highest = -1;
            for (DWORD i = 0; i < count; ++i)
                if ((cells[i * 4 + 3] & 31) == 3) {
                    hasSolid = true;
                    highest = i;
                }
            bool plain = plainWater(cells);
            int height = bz, newBase = bz;
            Bytes next;
            if (hasSolid && !plain) {
                next = liquidize(cells);
                height = bz + highest;
            } else {
                Bytes original = cells;
                if (!plain) {
                    auto known = converted.find({nx, ny});
                    if (known == converted.end())
                        continue;
                    height = known->second;
                    int wet = 0;
                    while (wet < (int)count && (cells[wet * 4 + 3] & 31) == 2)
                        ++wet;
                    bool valid = wet > 0 && bz + wet - 1 == height;
                    for (int i = wet; i < (int)count; ++i)
                        if (cells[i * 4 + 3] & 31)
                            valid = false;
                    if (!valid)
                        continue;
                    std::array<unsigned char, 4> air{{255, 255, 255, 0}};
                    if (wet < (int)count)
                        memcpy(air.data(), cells.data() + wet * 4, 4);
                    original[3] = (cells[3] & 224) | 3;
                    for (DWORD i = 1; i < count; ++i)
                        memcpy(original.data() + i * 4, air.data(), 4);
                }
                int d = std::min(depth(nx, ny, height), (int)count);
                next = deepen(original, d);
                newBase = height - (d - 1);
            }
            if (next.empty() || (next == cells && newBase == bz))
                continue;
            auto dirty = flags(nx, ny);
            if (dirty.empty())
                continue;
            changes.push_back({nx, ny, height, bz, newBase, cols + (nx % 256 + ny % 256 * 256) * 32,
                               data, count, cells, next, dirty});
        }
        if (work.cursor == 1024) {
            work.cursor = 0;
            work.completed = GetTickCount64();
        }
        if (changes.empty())
            return 0;
        if (suspend(process) != 0)
            throw std::runtime_error("Could not pause terrain safely");
        std::vector<Change> valid;
        size_t done = 0;
        try {
            for (auto &c : changes)
                if (ptr(c.column + 24) == c.data && ptr(c.column + 28) == c.count &&
                    read(c.column + 16, 4) == intBytes(c.oldBase) &&
                    read(c.data, c.count * 4) == c.before && flags(c.x, c.y, true) == c.flags)
                    valid.push_back(c);
            if (!valid.empty() && !journal(valid))
                throw std::runtime_error("Could not journal terrain originals");
            for (auto &c : valid) {
                ++done;
                if (!write(c.data, c.after) || !write(c.column + 16, intBytes(c.newBase)))
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
                write(c.column + 16, intBytes(c.oldBase));
            }
            resume(process);
            throw;
        }
        resume(process);
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
        log("Native water + shore reeds worker active: 2-3 depth, stones preserved, no added root geometry; loaded chunks converted as they appear");
        // 1 ms sleeps while work remains; the default 15.6 ms timer would
        // otherwise add a frame of delay to every pass.
        timeBeginPeriod(1);
        DWORD code = 0;
        int failures = 0;
        while (GetExitCodeProcess(patch.process, &code) && code == STILL_ACTIVE) {
            bool busy = false;
            try {
                patch.observeLoadedChunks();
                // Water first: swimmable terrain is what the player touches.
                busy = patch.convertWater(8);
                patch.relocateReeds();
                patch.repairReedOwners();
                busy = busy || patch.reedBacklog;
                failures = 0;
            } catch (const std::exception &e) {
                // Each batch rolls back its own writes before throwing, so a
                // transient failure skips one pass instead of ending the worker.
                log(e.what());
                if (++failures >= 10)
                    throw;
                Sleep(250);
            }
            Sleep(busy ? 1 : 5);
        }
        timeEndPeriod(1);
        log("Water worker stopped with game");
    } catch (const std::exception &e) {
        log(e.what());
    } catch (...) {
        log("Water worker stopped after unexpected failure");
    }
    CloseHandle(mutex);
}
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
