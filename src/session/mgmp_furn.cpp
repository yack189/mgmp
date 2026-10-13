// mgmp_furn.cpp -- see mgmp_furn.h.

#include "mgmp_furn.h"
#include "mgmp_house.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_net.h"

#include <windows.h>
#include <cstring>

namespace mgmp {
namespace {

using fn_scene_list = void  (__fastcall*)(void* scene, int type);
using fn_this       = void  (__fastcall*)(void* self);
using fn_new_entity = void* (__fastcall*)(void* scene);
using fn_create     = void* (__fastcall*)(void* scene, void* entity, uint64_t* record_id);
using fn_str_assign = void* (__fastcall*)(void* str, const char* chars, size_t len);

// Component: +0x20 the scene. Scene: +0x20 the typed component lists (see
// mgmp_house), +0x4B0 "being torn down" -- sub_14096B3E0 and sub_1401ACA10
// both refuse to build anything while it is set.
constexpr uintptr_t kComp_Scene     = 0x20;
constexpr uintptr_t kScene_Lists    = 0x20;
constexpr uintptr_t kScene_Dying    = 0x4B0;
constexpr int       kType_Piece     = 0x3F0;

// HouseInventory: {cap @0x38, count @0x3C, Record** @0x40}.
constexpr uintptr_t kInv_Count  = 0x3C;
constexpr uintptr_t kInv_Data   = 0x40;
// The record (0x68 bytes, sub_140205B00).
constexpr uintptr_t kRec_Id     = 0x00;
constexpr uintptr_t kRec_Name   = 0x08;
constexpr uintptr_t kRec_Grid   = 0x30;
constexpr uintptr_t kRec_Cell   = 0x50;
constexpr uintptr_t kRec_Orient = 0x58;
constexpr uintptr_t kRec_Order  = 0x60;
// FurniturePiece.
constexpr uintptr_t kFP_Grid    = 0x48;    // placed on, or null
constexpr uintptr_t kFP_Held    = 0x2B0;   // in the player's hand
constexpr uintptr_t kFP_Carried = 0x2B1;   // riding on a piece in the hand
constexpr uintptr_t kFP_Record  = 0x2D8;

constexpr uint32_t  kMaxRecords = 768;
constexpr uint32_t  kMaxPieces  = 768;
constexpr uint32_t  kMaxQueue   = 64;
constexpr ULONGLONG kScanMs     = 200;

struct Loc {
    char    grid[32] = {};
    int32_t x = 0, y = 0;
    int32_t ox = 1, oy = 1;
};

bool same_loc(const Loc& a, const Loc& b) {
    return strcmp(a.grid, b.grid) == 0 && a.x == b.x && a.y == b.y &&
           a.ox == b.ox && a.oy == b.oy;
}

struct Base {
    uint64_t id = 0;
    Loc      loc;
    bool     seen = false;
};

struct PieceOf { void* rec; void* piece; bool hand; };

struct State {
    bool          ok = false;
    fn_scene_list scene_list = nullptr;
    fn_this       unplace = nullptr;
    fn_this       destroy = nullptr;
    fn_new_entity new_entity = nullptr;
    fn_create     create = nullptr;
    fn_str_assign str_assign = nullptr;
    void**        inv_slot = nullptr;

    void*     house = nullptr;           // the house the baseline belongs to
    Base      base[kMaxRecords];
    uint32_t  nbase = 0;
    bool      based = false;
    ULONGLONG scan_at = 0;

    FurnMsg   q[kMaxQueue];
    uint32_t  nq = 0;

    uint32_t  sent = 0, got = 0, applied = 0, missed = 0;
};

State g;

bool read_str(const void* str, char* out, size_t cap) {
    return mem_read_std_string(str, out, cap);
}

void* inventory() {
    void* inv = nullptr;
    if (!g.inv_slot || !mem_read(g.inv_slot, &inv, sizeof(inv))) return nullptr;
    return inv;
}

uint32_t records(void**& data) {
    data = nullptr;
    void* inv = inventory();
    uint32_t n = 0;
    if (!inv || !mem_read((const uint8_t*)inv + kInv_Count, &n, sizeof(n))) return 0;
    if (!mem_read((const uint8_t*)inv + kInv_Data, &data, sizeof(data)) || !data) return 0;
    return n > kMaxRecords ? kMaxRecords : n;
}

bool read_loc(const void* rec, Loc& l) {
    l = Loc{};
    char grid[64];
    if (!read_str((const uint8_t*)rec + kRec_Grid, grid, sizeof(grid))) return false;
    if (strlen(grid) >= sizeof(l.grid)) return false;      // cannot travel; say nothing
    strncpy_s(l.grid, grid, _TRUNCATE);
    int32_t c[2] = {}, o[2] = {1, 1};
    mem_read((const uint8_t*)rec + kRec_Cell, c, sizeof(c));
    mem_read((const uint8_t*)rec + kRec_Orient, o, sizeof(o));
    if (l.grid[0]) { l.x = c[0]; l.y = c[1]; l.ox = o[0]; l.oy = o[1]; }
    return true;
}

// Every piece in the scene, with the record it shows and whether a hand has it.
uint32_t pieces(void* scene, PieceOf* out, uint32_t cap) {
    g.scene_list(scene, kType_Piece);
    void* table = nullptr; void* list = nullptr; void** data = nullptr;
    uint32_t n = 0, k = 0;
    if (!mem_read((const uint8_t*)scene + kScene_Lists, &table, sizeof(table)) || !table) return 0;
    if (!mem_read((const uint8_t*)table + (uintptr_t)kType_Piece * 0x10, &list, sizeof(list)) ||
        !list) return 0;
    if (!mem_read((const uint8_t*)list + 0xC, &n, sizeof(n))) return 0;
    if (!mem_read((const uint8_t*)list + 0x10, &data, sizeof(data)) || !data) return 0;
    for (uint32_t i = 0; i < n && k < cap; ++i) {
        void* p = nullptr;
        if (!mem_read(data + i, &p, sizeof(p)) || !p) continue;
        PieceOf& o = out[k];
        o.piece = p;
        o.rec = nullptr;
        mem_read((const uint8_t*)p + kFP_Record, &o.rec, sizeof(o.rec));
        uint8_t held = 0, carried = 0;
        mem_read((const uint8_t*)p + kFP_Held, &held, 1);
        mem_read((const uint8_t*)p + kFP_Carried, &carried, 1);
        o.hand = held || carried;
        if (o.rec) ++k;
    }
    return k;
}

const PieceOf* piece_of(const PieceOf* ps, uint32_t n, const void* rec) {
    for (uint32_t i = 0; i < n; ++i) if (ps[i].rec == rec) return &ps[i];
    return nullptr;
}

Base* base_find(uint64_t id) {
    for (uint32_t i = 0; i < g.nbase; ++i) if (g.base[i].id == id) return &g.base[i];
    return nullptr;
}

void base_set(uint64_t id, const Loc& l) {
    Base* b = base_find(id);
    if (!b) {
        if (g.nbase >= kMaxRecords) return;
        b = &g.base[g.nbase++];
        b->id = id;
    }
    b->loc = l;
    b->seen = true;
}

bool skip_name(const char* nm) { return !nm[0] || strcmp(nm, "poop") == 0; }

bool quiet() {
    // The new day is coming as a save: nothing said or done now would outlive it.
    return house_reload_pending() || house_night_pending() || house_night_replaying();
}

// --- watching this player --------------------------------------------------------

void scan(void* scene) {
    static PieceOf ps[kMaxPieces];
    const uint32_t np = pieces(scene, ps, kMaxPieces);
    void** data = nullptr;
    const uint32_t n = records(data);
    for (uint32_t i = 0; i < g.nbase; ++i) g.base[i].seen = false;

    for (uint32_t i = 0; i < n; ++i) {
        void* rec = nullptr;
        if (!mem_read(data + i, &rec, sizeof(rec)) || !rec) continue;
        uint64_t id = 0;
        char name[48];
        if (!mem_read((const uint8_t*)rec + kRec_Id, &id, sizeof(id))) continue;
        if (!read_str((const uint8_t*)rec + kRec_Name, name, sizeof(name)) || skip_name(name))
            continue;
        Loc now;
        if (!read_loc(rec, now)) continue;
        Base* b = base_find(id);
        if (!b || !g.based) { base_set(id, now); continue; }
        b->seen = true;
        if (same_loc(b->loc, now)) continue;
        const PieceOf* p = piece_of(ps, np, rec);
        if (p && p->hand) continue;              // still being moved
        if (!now.grid[0] && p) continue;         // put down off any grid: floating
        if (strlen(name) >= sizeof(FurnMsg{}.name)) { b->loc = now; continue; }

        FurnMsg m{};
        strncpy_s(m.name, name, _TRUNCATE);
        strncpy_s(m.from_grid, b->loc.grid, _TRUNCATE);
        m.from_x = b->loc.x; m.from_y = b->loc.y;
        strncpy_s(m.to_grid, now.grid, _TRUNCATE);
        m.to_x = now.x; m.to_y = now.y;
        m.orient_x = now.ox; m.orient_y = now.oy;
        mem_read((const uint8_t*)rec + kRec_Order, &m.order, sizeof(m.order));
        if (net_send_furn(m)) ++g.sent;
        log_line_lvl(LogLevel::Trace, "FURN", "-> '%s' %s(%d,%d) -> %s(%d,%d)", name,
                     m.from_grid[0] ? m.from_grid : "storage", m.from_x, m.from_y,
                     m.to_grid[0] ? m.to_grid : "storage", m.to_x, m.to_y);
        b->loc = now;
    }
    // Records gone (a sale, the night) leave the baseline.
    uint32_t w = 0;
    for (uint32_t i = 0; i < g.nbase; ++i) if (g.base[i].seen) g.base[w++] = g.base[i];
    g.nbase = w;
    g.based = true;
}

// --- doing what the other player did ---------------------------------------------

bool apply(void* scene, const FurnMsg& m) {
    static PieceOf ps[kMaxPieces];
    const uint32_t np = pieces(scene, ps, kMaxPieces);
    void** data = nullptr;
    const uint32_t n = records(data);

    void* found = nullptr;
    const PieceOf* fp = nullptr;
    bool already = false;
    for (uint32_t i = 0; i < n && !found; ++i) {
        void* rec = nullptr;
        char name[48];
        if (!mem_read(data + i, &rec, sizeof(rec)) || !rec) continue;
        if (!read_str((const uint8_t*)rec + kRec_Name, name, sizeof(name)) ||
            strcmp(name, m.name) != 0) continue;
        Loc l;
        if (!read_loc(rec, l)) continue;
        const PieceOf* p = piece_of(ps, np, rec);
        if (p && p->hand) continue;              // this player has that one in hand
        if (m.to_grid[0] && strcmp(l.grid, m.to_grid) == 0 && l.x == m.to_x && l.y == m.to_y)
            already = true;
        const bool from_storage = !m.from_grid[0];
        if (from_storage ? (!l.grid[0] && !p)
                         : (strcmp(l.grid, m.from_grid) == 0 && l.x == m.from_x &&
                            l.y == m.from_y)) {
            found = rec;
            fp = p;
        }
    }
    if (!found) {
        if (!already) {
            ++g.missed;
            log_line("FURN", "!! the other player moved '%s' from %s(%d,%d), and this house"
                             " has no such piece there -- left as it is", m.name,
                     m.from_grid[0] ? m.from_grid : "storage", m.from_x, m.from_y);
        }
        return false;
    }

    // Put the piece it shows away: off its grid, then gone -- exactly what the
    // game does to a piece dropped back on the build menu.
    if (fp) {
        void* grid = nullptr;
        mem_read((const uint8_t*)fp->piece + kFP_Grid, &grid, sizeof(grid));
        if (grid) g.unplace(fp->piece);
        g.destroy(fp->piece);
    }

    // The record is the furniture. Rewrite it...
    g.str_assign((uint8_t*)found + kRec_Grid, m.to_grid, strlen(m.to_grid));
    if (m.to_grid[0]) {
        const int32_t c[2] = { m.to_x, m.to_y }, o[2] = { m.orient_x, m.orient_y };
        mem_write((uint8_t*)found + kRec_Cell, c, sizeof(c));
        mem_write((uint8_t*)found + kRec_Orient, o, sizeof(o));
        mem_write((uint8_t*)found + kRec_Order, &m.order, sizeof(m.order));
    }
    uint64_t id = 0;
    mem_read((const uint8_t*)found + kRec_Id, &id, sizeof(id));

    // ...and if it stands somewhere, build its piece again; the piece's own init
    // finds the grid by name and places itself, as at load.
    if (m.to_grid[0]) {
        uint8_t dying = 1;
        mem_read((const uint8_t*)scene + kScene_Dying, &dying, 1);
        if (!dying) {
            void* ent = g.new_entity(scene);
            if (ent) g.create(scene, ent, &id);
        }
    }
    Loc l;
    if (read_loc(found, l)) base_set(id, l);
    ++g.applied;
    log_line_lvl(LogLevel::Trace, "FURN", "<- '%s' %s(%d,%d) -> %s(%d,%d), as the other"
                 " player did", m.name, m.from_grid[0] ? m.from_grid : "storage", m.from_x,
                 m.from_y, m.to_grid[0] ? m.to_grid : "storage", m.to_x, m.to_y);
    return true;
}

} // namespace

void furn_set_base(uintptr_t) {
    g.scene_list = (fn_scene_list)addr_of_call(C_SceneList);
    g.unplace    = (fn_this)      addr_of_call(C_FurnUnplace);
    g.destroy    = (fn_this)      addr_of_call(C_CompDestroy);
    g.new_entity = (fn_new_entity)addr_of_call(C_SceneNewEntity);
    g.create     = (fn_create)    addr_of_call(C_FurnCreate);
    g.str_assign = (fn_str_assign)addr_of_call(C_StrAssign);
    g.inv_slot   = (void**)       addr_of_data(D_HouseInventory);
    g.ok = g.scene_list && g.unplace && g.destroy && g.new_entity && g.create &&
           g.str_assign && g.inv_slot;
    if (!g.ok)
        log_line("FURN", "!! the furniture functions did not resolve -- each player's"
                         " furniture stays their own");
}

void furn_on_house_update(void* house) {
    if (!g.ok || !house || !net_active()) return;
    void* scene = nullptr;
    if (!mem_read((const uint8_t*)house + kComp_Scene, &scene, sizeof(scene)) || !scene) return;
    if (house != g.house) {
        // A house built from a save: a fresh set of records with fresh ids.
        g.house = house;
        g.nbase = 0;
        g.based = false;
        g.nq = 0;
    }
    if (quiet()) { g.nq = 0; g.based = false; return; }

    // The other player's first, so this player's scan sees them as already known.
    for (uint32_t i = 0; i < g.nq; ++i) apply(scene, g.q[i]);
    g.nq = 0;

    const ULONGLONG now = GetTickCount64();
    if (now - g.scan_at < kScanMs) return;
    g.scan_at = now;
    scan(scene);
}

void furn_on_message(uint8_t, const FurnMsg& m) {
    if (!g.ok) return;
    ++g.got;
    if (g.nq >= kMaxQueue) {
        log_line("FURN", "!! %u furniture moves already waiting -- dropping '%s'", g.nq, m.name);
        return;
    }
    g.q[g.nq++] = m;
}

void furn_shutdown() {
    if (g.sent || g.got)
        log_line_lvl(g.missed ? LogLevel::Warn : LogLevel::Trace, "FURN",
                     "done: %u move(s) sent, %u received, %u applied, %u not found",
                     g.sent, g.got, g.applied, g.missed);
    g.nbase = 0; g.based = false; g.nq = 0; g.house = nullptr;
}

} // namespace mgmp
