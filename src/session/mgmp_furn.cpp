// mgmp_furn.cpp -- see mgmp_furn.h.

#include "mgmp_furn.h"
#include "mgmp_house.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_town.h"

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
// The piece's own Renderer (made by its init through sub_14005A3D0), and the
// alpha the Renderer base ctor (sub_14005A5D0) sets to 1.0 -- a plain multiply
// in the consumer, nothing else writes it for a piece.
constexpr uintptr_t kFP_Renderer = 0x40;
constexpr uintptr_t kRend_Alpha  = 0x60;
constexpr double    kGhostAlpha  = 0.35;
// HouseInventory: the house's wallet. Shop::BuyItem reads +0xB4 when the shop
// is a house shop (Shop+0x7B); ObtainItem's Food/Coins add to +0xB0/+0xB4.
constexpr uintptr_t kInv_Food  = 0xB0;
constexpr uintptr_t kInv_Coins = 0xB4;

constexpr uint32_t  kMaxRecords = 768;
constexpr uint32_t  kMaxPieces  = 768;
constexpr uint32_t  kMaxQueue   = 64;
constexpr ULONGLONG kScanMs     = 200;
constexpr uint32_t  kMaxHolds   = 64;
constexpr uint32_t  kMaxGhosts  = 128;
constexpr uint32_t  kSentRing   = 32;
// A hold the other player never let go of (a crash, a dropped line).
constexpr ULONGLONG kHoldMaxMs  = 90000;
// Two players who moved the same piece within this long of each other: the
// host's placement wins.
constexpr ULONGLONG kRaceMs     = 30000;
constexpr ULONGLONG kPurseBeatMs = 2000;
constexpr ULONGLONG kPurseStaleMs = 10000;

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

// This player has it in hand, and the other was told (from = where it stood).
struct HeldSent { uint64_t id; char name[40]; Loc loc; };
// The other player has it in hand: it is ghosted here and cannot be picked up.
struct Lock     { char name[40]; Loc loc; ULONGLONG at; };
struct Ghost    { void* piece; double alpha; };
// The host's own recent moves, to settle a piece both players moved at once.
struct SentMove { char name[40]; Loc from, to; ULONGLONG at; };

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

    HeldSent  held[kMaxHolds];
    uint32_t  nheld = 0;
    Lock      locks[kMaxHolds];
    uint32_t  nlocks = 0;
    Ghost     ghosts[kMaxGhosts];
    uint32_t  nghosts = 0;
    SentMove  ring[kSentRing];
    uint32_t  ring_n = 0;

    // the wallet: host what it last sent, client what it last heard
    int32_t   purse_coins = 0, purse_food = 0;
    bool      purse_have = false;
    ULONGLONG purse_at = 0;
    bool      said_purse = false;

    uint32_t  sent = 0, got = 0, applied = 0, missed = 0;
    uint32_t  holds_sent = 0, holds_got = 0, refused = 0, settled = 0, purse_fixed = 0;
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

bool is_host()   { return net_role() == NetRole::Host; }
bool is_client() { return net_role() == NetRole::Client; }

// The same spot: the same grid cell, or both the storage.
bool same_place(const char* ga, int32_t xa, int32_t ya, const char* gb, int32_t xb, int32_t yb) {
    if (strcmp(ga, gb) != 0) return false;
    return !ga[0] || (xa == xb && ya == yb);
}

const char* where(const char* grid) { return grid[0] ? grid : "storage"; }

void* renderer_of(void* piece) {
    void* r = nullptr;
    mem_read((const uint8_t*)piece + kFP_Renderer, &r, sizeof(r));
    return r;
}

uint64_t id_of(const void* rec) {
    uint64_t id = 0;
    mem_read((const uint8_t*)rec + kRec_Id, &id, sizeof(id));
    return id;
}

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
        if (is_host()) {
            SentMove& e = g.ring[g.ring_n++ % kSentRing];
            strncpy_s(e.name, name, _TRUNCATE);
            e.from = b->loc;
            e.to = now;
            e.at = GetTickCount64();
        }
        log_line_lvl(LogLevel::Trace, "FURN", "-> '%s' %s(%d,%d) -> %s(%d,%d)", name,
                     m.from_grid[0] ? m.from_grid : "storage", m.from_x, m.from_y,
                     m.to_grid[0] ? m.to_grid : "storage", m.to_x, m.to_y);
        b->loc = now;
    }
    // Put down: say so, after the move (if any) the loop above just sent.
    for (uint32_t i = 0; i < g.nheld;) {
        bool still = false;
        for (uint32_t k = 0; k < np && !still; ++k)
            still = ps[k].hand && id_of(ps[k].rec) == g.held[i].id;
        if (still) { ++i; continue; }
        FurnMsg m{};
        m.op = kFurnRelease;
        strncpy_s(m.name, g.held[i].name, _TRUNCATE);
        strncpy_s(m.from_grid, g.held[i].loc.grid, _TRUNCATE);
        m.from_x = g.held[i].loc.x; m.from_y = g.held[i].loc.y;
        net_send_furn(m);
        g.held[i] = g.held[--g.nheld];
    }
    // Records gone (a sale, the night) leave the baseline.
    uint32_t w = 0;
    for (uint32_t i = 0; i < g.nbase; ++i) if (g.base[i].seen) g.base[w++] = g.base[i];
    g.nbase = w;
    g.based = true;
}

// --- doing what the other player did ---------------------------------------------

// Host: the client moved a piece from where the host had just moved it from --
// both picked it up before either heard the other's hold. The host's
// placement wins: the client is told to move its copy from where IT put it to
// where the host did.
bool settle(const FurnMsg& m, void** data, uint32_t n, const PieceOf* ps, uint32_t np) {
    const ULONGLONG now = GetTickCount64();
    const uint32_t cnt = g.ring_n < kSentRing ? g.ring_n : kSentRing;
    for (uint32_t j = 0; j < cnt; ++j) {
        const SentMove& e = g.ring[(g.ring_n - 1 - j) % kSentRing];
        if (now - e.at > kRaceMs) break;
        if (strcmp(e.name, m.name) != 0 ||
            !same_place(e.from.grid, e.from.x, e.from.y, m.from_grid, m.from_x, m.from_y))
            continue;
        // Where the host's copy stands now (it may have moved on since).
        for (uint32_t i = 0; i < n; ++i) {
            void* rec = nullptr;
            char name[48];
            if (!mem_read(data + i, &rec, sizeof(rec)) || !rec) continue;
            if (!read_str((const uint8_t*)rec + kRec_Name, name, sizeof(name)) ||
                strcmp(name, m.name) != 0) continue;
            Loc l;
            if (!read_loc(rec, l)) continue;
            const PieceOf* p = piece_of(ps, np, rec);
            if (p && p->hand) continue;
            if (!same_place(l.grid, l.x, l.y, e.to.grid, e.to.x, e.to.y)) continue;
            if (!l.grid[0] && p) continue;
            FurnMsg c{};
            c.op = kFurnMove;
            strncpy_s(c.name, m.name, _TRUNCATE);
            strncpy_s(c.from_grid, m.to_grid, _TRUNCATE);
            c.from_x = m.to_x; c.from_y = m.to_y;
            strncpy_s(c.to_grid, l.grid, _TRUNCATE);
            c.to_x = l.x; c.to_y = l.y;
            c.orient_x = l.ox; c.orient_y = l.oy;
            mem_read((const uint8_t*)rec + kRec_Order, &c.order, sizeof(c.order));
            net_send_furn(c);
            ++g.settled;
            log_line("FURN", "both players moved '%s' at once -- it stays where the host put"
                             " it, %s(%d,%d); the other player's copy follows", m.name,
                     where(l.grid), l.x, l.y);
            return true;
        }
        return false;
    }
    return false;
}

void lock_drop(const char* name, const char* grid, int32_t x, int32_t y) {
    for (uint32_t i = 0; i < g.nlocks;) {
        const Lock& k = g.locks[i];
        if (strcmp(k.name, name) == 0 && same_place(k.loc.grid, k.loc.x, k.loc.y, grid, x, y))
            g.locks[i] = g.locks[--g.nlocks];
        else ++i;
    }
}

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
        if (already) return false;
        if (is_host() && settle(m, data, n, ps, np)) return false;
        ++g.missed;
        log_line_lvl(is_client() ? LogLevel::Trace : LogLevel::Warn, "FURN",
                 "the other player moved '%s' from %s(%d,%d), and this house has no such"
                 " piece there -- left as it is%s", m.name, where(m.from_grid), m.from_x,
                 m.from_y, is_client() ? " (if this player moved it too, the host's"
                 " placement is on its way)" : "");
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

void handle(void* scene, const FurnMsg& m) {
    switch (m.op) {
    case kFurnMove:
        apply(scene, m);
        lock_drop(m.name, m.from_grid, m.from_x, m.from_y);
        break;
    case kFurnHold:
        if (!m.from_grid[0]) break;
        lock_drop(m.name, m.from_grid, m.from_x, m.from_y);
        if (g.nlocks < kMaxHolds) {
            Lock& k = g.locks[g.nlocks++];
            strncpy_s(k.name, m.name, _TRUNCATE);
            k.loc = Loc{};
            strncpy_s(k.loc.grid, m.from_grid, _TRUNCATE);
            k.loc.x = m.from_x; k.loc.y = m.from_y;
            k.at = GetTickCount64();
        }
        ++g.holds_got;
        log_line_lvl(LogLevel::Trace, "FURN", "<- the other player picked up '%s' at %s(%d,%d)"
                     " -- ghosted here until they put it down", m.name, m.from_grid,
                     m.from_x, m.from_y);
        break;
    case kFurnRelease:
        lock_drop(m.name, m.from_grid, m.from_x, m.from_y);
        break;
    default:
        break;
    }
}

// A piece this player just picked up off a grid: tell the other player, who
// ghosts it, before either can put it somewhere the other did not.
void watch_hands(void* scene) {
    static PieceOf ps[kMaxPieces];
    const uint32_t np = pieces(scene, ps, kMaxPieces);
    for (uint32_t i = 0; i < np; ++i) {
        if (!ps[i].hand) continue;
        const uint64_t id = id_of(ps[i].rec);
        bool known = false;
        for (uint32_t k = 0; k < g.nheld && !known; ++k) known = g.held[k].id == id;
        if (known || g.nheld >= kMaxHolds) continue;
        const Base* b = base_find(id);
        if (!b || !b->loc.grid[0]) continue;     // out of storage: nothing to hold
        char name[48];
        if (!read_str((const uint8_t*)ps[i].rec + kRec_Name, name, sizeof(name)) ||
            skip_name(name) || strlen(name) >= sizeof(FurnMsg{}.name))
            continue;
        HeldSent& h = g.held[g.nheld++];
        h.id = id;
        strncpy_s(h.name, name, _TRUNCATE);
        h.loc = b->loc;
        FurnMsg m{};
        m.op = kFurnHold;
        strncpy_s(m.name, name, _TRUNCATE);
        strncpy_s(m.from_grid, b->loc.grid, _TRUNCATE);
        m.from_x = b->loc.x; m.from_y = b->loc.y;
        if (net_send_furn(m)) ++g.holds_sent;
    }
}

void ghost_set(void* piece, double a) {
    if (void* r = renderer_of(piece)) mem_write((uint8_t*)r + kRend_Alpha, &a, sizeof(a));
}

// The pieces the other player has in hand: see-through here, and refused to
// this player's own hand (furn_pickup_allowed).
void ghost(void* scene) {
    static PieceOf ps[kMaxPieces];
    const uint32_t np = (g.nlocks || g.nghosts) ? pieces(scene, ps, kMaxPieces) : 0;
    const ULONGLONG now = GetTickCount64();
    for (uint32_t i = 0; i < g.nlocks;) {
        if (now - g.locks[i].at > kHoldMaxMs) {
            log_line("FURN", "!! '%s' has been in the other player's hand for %llu s -- not"
                             " holding it for them any longer", g.locks[i].name,
                     (unsigned long long)(kHoldMaxMs / 1000));
            g.locks[i] = g.locks[--g.nlocks];
        } else ++i;
    }
    Ghost next[kMaxGhosts];
    uint32_t nn = 0;
    if (g.nlocks) {
        void** data = nullptr;
        const uint32_t n = records(data);
        for (uint32_t i = 0; i < n && nn < kMaxGhosts; ++i) {
            void* rec = nullptr;
            char name[48];
            if (!mem_read(data + i, &rec, sizeof(rec)) || !rec) continue;
            if (!read_str((const uint8_t*)rec + kRec_Name, name, sizeof(name))) continue;
            bool locked = false;
            Loc l;
            for (uint32_t k = 0; k < g.nlocks && !locked; ++k) {
                if (strcmp(g.locks[k].name, name) != 0) continue;
                if (!read_loc(rec, l) || !l.grid[0]) break;
                locked = same_place(l.grid, l.x, l.y, g.locks[k].loc.grid, g.locks[k].loc.x,
                                    g.locks[k].loc.y);
            }
            if (!locked) continue;
            const PieceOf* p = piece_of(ps, np, rec);
            if (!p || p->hand) continue;
            next[nn++] = Ghost{ p->piece, 1.0 };
        }
    }
    // Let go of the ones no longer held, if they are still there to let go of.
    for (uint32_t i = 0; i < g.nghosts; ++i) {
        bool keep = false;
        for (uint32_t k = 0; k < nn && !keep; ++k) keep = next[k].piece == g.ghosts[i].piece;
        if (keep) continue;
        for (uint32_t k = 0; k < np; ++k)
            if (ps[k].piece == g.ghosts[i].piece) { ghost_set(ps[k].piece, g.ghosts[i].alpha); break; }
    }
    for (uint32_t k = 0; k < nn; ++k) {
        bool had = false;
        for (uint32_t i = 0; i < g.nghosts && !had; ++i)
            if (g.ghosts[i].piece == next[k].piece) { next[k].alpha = g.ghosts[i].alpha; had = true; }
        if (!had) {
            double a = 1.0;
            if (void* r = renderer_of(next[k].piece))
                mem_read((const uint8_t*)r + kRend_Alpha, &a, sizeof(a));
            next[k].alpha = (a > 0.0 && a <= 1.0) ? a : 1.0;
        }
        ghost_set(next[k].piece, kGhostAlpha);
    }
    memcpy(g.ghosts, next, nn * sizeof(Ghost));
    g.nghosts = nn;
}

int32_t inv_i32(uintptr_t off) {
    int32_t v = 0;
    if (void* inv = inventory()) mem_read((const uint8_t*)inv + off, &v, sizeof(v));
    return v;
}

// Host: the house's coins and food, when they change and every couple of
// seconds besides.
void purse_send(ULONGLONG now) {
    if (!is_host() || !inventory()) return;
    const int32_t coins = inv_i32(kInv_Coins), food = inv_i32(kInv_Food);
    if (g.purse_have && coins == g.purse_coins && food == g.purse_food &&
        now - g.purse_at < kPurseBeatMs)
        return;
    FurnMsg m{};
    m.op = kFurnPurse;
    m.coins = coins;
    m.food = food;
    if (net_send_furn(m)) {
        g.purse_coins = coins; g.purse_food = food;
        g.purse_have = true; g.purse_at = now;
    }
}

// Client: one wallet, the host's. Not while a press of the host's is still
// to be made here -- that press is about to spend what the host already spent.
void purse_follow() {
    if (!is_client() || !g.purse_have || town_presses_pending()) return;
    if (GetTickCount64() - g.purse_at > kPurseStaleMs) return;
    void* inv = inventory();
    if (!inv) return;
    const int32_t coins = inv_i32(kInv_Coins), food = inv_i32(kInv_Food);
    if (coins == g.purse_coins && food == g.purse_food) return;
    mem_write((uint8_t*)inv + kInv_Coins, &g.purse_coins, sizeof(int32_t));
    mem_write((uint8_t*)inv + kInv_Food, &g.purse_food, sizeof(int32_t));
    ++g.purse_fixed;
    if (!g.said_purse) {
        g.said_purse = true;
        log_line("FURN", "the house's coins and food are the host's -- %d coins here became %d",
                 coins, g.purse_coins);
    } else {
        log_line_lvl(LogLevel::Trace, "FURN", "coins %d -> %d, food %d -> %d (the host's)",
                     coins, g.purse_coins, food, g.purse_food);
    }
}

void forget_holds() {
    g.nheld = 0;
    g.nlocks = 0;
    g.nghosts = 0;           // the pieces are gone with the house, or about to be
}

} // namespace

bool furn_pickup_allowed(void* piece) {
    if (!piece) return true;
    for (uint32_t i = 0; i < g.nghosts; ++i) {
        if (g.ghosts[i].piece != piece) continue;
        if (g.refused++ % 20 == 0)
            log_line("FURN", "the other player has that piece in hand -- it can be moved once"
                             " they put it down");
        return false;
    }
    return true;
}

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
        forget_holds();
    }
    if (quiet()) { g.nq = 0; g.based = false; forget_holds(); return; }

    // The other player's first, so this player's scan sees them as already known.
    for (uint32_t i = 0; i < g.nq; ++i) handle(scene, g.q[i]);
    g.nq = 0;

    watch_hands(scene);
    ghost(scene);
    purse_follow();

    const ULONGLONG now = GetTickCount64();
    if (now - g.scan_at < kScanMs) return;
    g.scan_at = now;
    scan(scene);
    purse_send(now);
}

void furn_on_message(uint8_t, const FurnMsg& m) {
    if (!g.ok) return;
    if (m.op == kFurnPurse) {
        // Taken at once, not queued: it is the newest word on the wallet.
        if (!is_client()) return;
        g.purse_coins = m.coins;
        g.purse_food = m.food;
        g.purse_have = true;
        g.purse_at = GetTickCount64();
        return;
    }
    if (m.op == kFurnMove) ++g.got;
    if (g.nq >= kMaxQueue) {
        log_line("FURN", "!! %u furniture moves already waiting -- dropping '%s'", g.nq, m.name);
        return;
    }
    g.q[g.nq++] = m;
}

void furn_shutdown() {
    if (g.sent || g.got || g.holds_sent || g.holds_got)
        log_line_lvl(g.missed ? LogLevel::Warn : LogLevel::Trace, "FURN",
                     "done: %u move(s) sent, %u received, %u applied, %u not found, %u settled"
                     " for the host; %u pick-up(s) told, %u heard, %u refused here; wallet"
                     " followed %u time(s)", g.sent, g.got, g.applied, g.missed, g.settled,
                     g.holds_sent, g.holds_got, g.refused, g.purse_fixed);
    g.nbase = 0; g.based = false; g.nq = 0; g.house = nullptr;
    forget_holds();
    g.purse_have = false;
}

} // namespace mgmp
