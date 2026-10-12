// mgmp_house.cpp -- see mgmp_house.h.

#include "mgmp_house.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_savefile.h"
#include "mgmp_leave.h"
#include "mgmp_ui.h"
#include "mgmp_follow.h"
#include "mgmp_rng.h"

#include <windows.h>
#include <cmath>
#include <cstring>

namespace mgmp {
namespace {

// Component+0x20 is the scene the component lives in: House::update passes it
// straight to the camera lookup (mov rcx,[rsi+20h] / call sub_140978330).
constexpr uintptr_t kComp_Scene = 0x20;

using fn_scene_cam   = void*   (__fastcall*)(void* scene);
using fn_mouse_world = double* (__fastcall*)(void* cam, double* out);
using fn_viewproj    = float*  (__fastcall*)(void* cam, float* out);
using fn_scene_list  = void    (__fastcall*)(void* scene, int type);
using fn_anim_set    = void    (__fastcall*)(void* animator, int pose, void* str);
using fn_area_op     = void    (__fastcall*)(void* area, void* cat);
using fn_str_dtor    = void    (__fastcall*)(void* str);
using fn_this        = void    (__fastcall*)(void* self);

// --- the scene's typed component lists ---------------------------------------
//
// sub_14096B470(scene, type) refreshes a cached vector of every live component
// of one type; the vector pointer then sits at *(scene+0x20) + type*0x10, as
// { i32 refcount; i32 _; u32 cap @8; u32 count @0xC; Component** data @0x10 }.
// Read off sub_1400ECF30 (type 0x449 -> +0x4490) and the HouseCat loop in
// sub_1400ADBF0 (type 0x448 -> +0x4480).
constexpr uintptr_t kScene_Lists     = 0x20;
constexpr int       kType_HouseCat   = 0x448;
constexpr int       kType_ClickMgr   = 0x449;
constexpr int       kType_PlaceArea  = 0x1D2;   // CatPlacementArea: box, beds...

// HouseCat
constexpr uintptr_t kHC_Phys   = 0x38;
constexpr uintptr_t kHC_Entity = 0x40;
constexpr uintptr_t kHC_Anim   = 0x68;
constexpr uintptr_t kHC_Id     = 0x80;
constexpr uintptr_t kHC_Area   = 0xE8;
constexpr uintptr_t kHC_Depth  = 0xF8;
// its entity (the cat's transform)
constexpr uintptr_t kEnt_X     = 0x80;
constexpr uintptr_t kEnt_Y     = 0x88;
constexpr uintptr_t kEnt_Z     = 0x90;
constexpr uintptr_t kEnt_Order = 0xB0;   // also +0x68: both are -(HouseCat+0xF8)
constexpr uintptr_t kEnt_Order2= 0x68;
constexpr uintptr_t kEnt_Flip  = 0xB8;
// its animator
constexpr uintptr_t kAn_Pose   = 0x148;
constexpr uintptr_t kAn_Face   = 0x14C;
constexpr uintptr_t kAn_Name   = 0x170;
// its physics
constexpr uintptr_t kPh_VX     = 0x48;
constexpr uintptr_t kPh_VY     = 0x50;
constexpr uintptr_t kPh_Ground = 0x69;
// HouseCatClickManager: the cat under the local mouse's grab
constexpr uintptr_t kCM_Held   = 0x38;

constexpr uint64_t kSendEveryMs   = 50;
constexpr uint64_t kSettleMs      = 350;    // at rest this long -> hand it back
constexpr uint64_t kSettleMaxMs   = 6000;   // ...or this long after release
constexpr uint64_t kStaleTargetMs = 3000;   // no word for this long -> run it locally
constexpr uint32_t kMaxAreas      = 32;

// --- poop ----------------------------------------------------------------------
// Read off the spawn, sub_1402EA9A0 (FurnitureGrid vtable slot 34):
//   piece = sub_1401ACA10(scene, ..., HouseInventory item "poop")
//   *(piece+0x38)+0x80/+0x88 = *(grid+0x38)+0x80/+0x88 + (double)cell
//   sub_1402EED50(piece, grid)              -- which stores grid at piece+0x48
// and off the clean-up's own name test: *(piece+0x2D8)+8 is the item's name.
constexpr int       kType_FurnGrid  = 0x3ED;
constexpr int       kType_FurnPiece = 0x3F0;
constexpr uintptr_t kFP_Entity  = 0x38;
constexpr uintptr_t kFP_Grid    = 0x48;
constexpr uintptr_t kFP_Meta    = 0x2D8;
constexpr uintptr_t kMeta_Name  = 0x08;
constexpr uintptr_t kFG_Entity  = 0x38;
constexpr uint32_t  kMaxPoops   = 256;
constexpr uint64_t  kPoopScanMs = 250;

struct Poop {
    void*   piece = nullptr;
    double  gx = 0, gy = 0;
    int32_t cx = 0, cy = 0;
};

// --- the night ---------------------------------------------------------------------
// The house's two streams, as mgmp_rng names them: +0x178 the simulation
// stream every night roll draws on, +0x198 (= +0x178 + 0x20) presentation.
constexpr uint64_t kNightNoInterMs = 20000;    // no night screens by then: over
constexpr uint64_t kNightMaxMs     = 300000;

struct Cat {
    void*    hc   = nullptr;
    void*    anim = nullptr;
    uint64_t id   = 0;
};

// Who drives a cat. Absent = the host.
struct Auth {
    uint64_t  id = 0;
    uint8_t   peer = kHostPeer;
    ULONGLONG released = 0;     // the holder let go at this tick (0 = still held)
    ULONGLONG still = 0;        // ...and it has been at rest since this tick
};

// The last state the authority sent for a cat, and what we are showing.
struct CatTarget {
    HouseCatState st;
    ULONGLONG at = 0;
    double    sx = 0, sy = 0, sz = 0;   // smoothed, what is actually written
    bool      placed = false;
    char      injected[24] = {};        // the last anim we put on it
    uint8_t   injected_pose = 0xFF;
    ULONGLONG injected_at = 0;
    uint8_t   area_tried = 0xFE;        // the last area index we tried to apply
};

struct State {
    bool           resolved    = false;
    fn_scene_cam   scene_cam   = nullptr;
    fn_mouse_world mouse_world = nullptr;
    fn_viewproj    viewproj    = nullptr;
    const float*   ndc         = nullptr;   // D_NdcRange, four floats

    ULONGLONG tick       = 0;
    double    mx = 0, my = 0;
    bool      have_mouse = false;
    float     vp[16]     = {};
    bool      have_vp    = false;

    bool said_live = false;
    bool said_proj = false;

    // --- the cats ---------------------------------------------------------
    bool           cats_ok     = false;
    fn_scene_list  scene_list  = nullptr;
    fn_anim_set    anim_set    = nullptr;
    fn_area_op     area_add    = nullptr;
    fn_area_op     area_remove = nullptr;
    fn_str_dtor    str_dtor    = nullptr;

    void*    house = nullptr;
    Cat      cats[kMaxHouseCats];
    uint32_t ncats = 0;
    void*    areas[kMaxAreas] = {};
    uint32_t nareas = 0;

    Auth     auth[kMaxHouseCats];
    uint32_t nauth = 0;
    uint64_t tgt_id[kMaxHouseCats] = {};
    CatTarget   tgt[kMaxHouseCats];
    uint32_t ntgt = 0;

    void*     held_prev = nullptr;
    uint64_t  held_prev_id = 0;
    ULONGLONG last_send = 0;
    bool      anim_injecting = false;
    LARGE_INTEGER qpc_freq{}, qpc_last{};
    double    frame_dt = 1.0 / 60.0;

    uint32_t sent = 0, got = 0, puppeted = 0, swallowed_anims = 0, areas_moved = 0;
    ULONGLONG sign_refused_at = 0;

    // The end of the day.
    ULONGLONG day_passed_at  = 0;     // host: PassDay ran; publish when settled
    ULONGLONG reload_at      = 0;     // client: told to reload into the new day
    ULONGLONG last_escape    = 0;
    uint32_t  escapes        = 0;
    uintptr_t base = 0;
    bool said_cats = false;

    // --- poop -------------------------------------------------------------
    bool      poop_ok      = false;
    fn_this   poop_spawn   = nullptr;   // C_PoopSpawn
    fn_this   poop_clean   = nullptr;   // C_PoopClean (through our own detour)
    Poop      poops[kMaxPoops];
    uint32_t  npoops       = 0;
    bool      poop_based   = false;     // host: poops[] is a baseline to diff
    ULONGLONG poop_scan_at = 0;
    bool      poop_injecting = false;
    bool      force_cell   = false;
    int32_t   force_cx = 0, force_cy = 0;
    uint32_t  poop_sent = 0, poop_got = 0, poop_missed = 0;

    // --- the night (client) ----------------------------------------------
    fn_this   end_day      = nullptr;   // C_HouseEndDay
    bool      night_go     = false;     // call EndDay on the next house tick
    bool      night_seeded = false;     // PassDay is to take these streams
    uint64_t  night_sim[4] = {}, night_pres[4] = {};
    ULONGLONG night_at     = 0;         // our own night is on screen
    bool      night_seen_inter = false;
    bool      host_after_ok = false, mine_after_ok = false;
    uint64_t  host_after[4] = {}, mine_after[4] = {};
};

State g;

// --- authority ------------------------------------------------------------------

uint8_t self_peer() {
    const uint8_t me = net_self();
    if (me != kNoPeer) return me;
    return net_role() == NetRole::Client ? kNoPeer : kHostPeer;
}

Auth* auth_find(uint64_t id) {
    for (uint32_t i = 0; i < g.nauth; ++i) if (g.auth[i].id == id) return &g.auth[i];
    return nullptr;
}
uint8_t authority_of(uint64_t id) {
    const Auth* a = auth_find(id);
    return a ? a->peer : kHostPeer;
}
void auth_set(uint64_t id, uint8_t peer) {
    Auth* a = auth_find(id);
    if (peer == kHostPeer) {                      // the default needs no entry
        if (a) *a = g.auth[--g.nauth];
        return;
    }
    if (!a) {
        if (g.nauth >= kMaxHouseCats) return;
        a = &g.auth[g.nauth++];
    }
    *a = Auth{};
    a->id = id;
    a->peer = peer;
}

CatTarget* tgt_find(uint64_t id, bool create) {
    for (uint32_t i = 0; i < g.ntgt; ++i) if (g.tgt_id[i] == id) return &g.tgt[i];
    if (!create || g.ntgt >= kMaxHouseCats) return nullptr;
    g.tgt_id[g.ntgt] = id;
    g.tgt[g.ntgt] = CatTarget{};
    return &g.tgt[g.ntgt++];
}

bool session_on() { return net_active() && g.cats_ok; }

// Is THIS peer showing someone else's version of this cat, rather than running
// it? Only with recent word from its authority: a cat nobody is describing is
// better run locally than frozen.
CatTarget* puppet_target(uint64_t id) {
    if (!session_on() || !house_active()) return nullptr;
    const uint8_t me = self_peer();
    if (me == kNoPeer || authority_of(id) == me) return nullptr;
    CatTarget* t = tgt_find(id, false);
    if (!t || !t->at || GetTickCount64() - t->at > kStaleTargetMs) return nullptr;
    return t;
}

// --- reading the house ------------------------------------------------------------

void* list_of(void* scene, int type, uint32_t& count, void**& data) {
    count = 0; data = nullptr;
    g.scene_list(scene, type);
    void* table = nullptr;
    if (!mem_read((const uint8_t*)scene + kScene_Lists, &table, sizeof(table)) || !table)
        return nullptr;
    void* list = nullptr;
    if (!mem_read((const uint8_t*)table + (uintptr_t)type * 0x10, &list, sizeof(list)) || !list)
        return nullptr;
    uint32_t n = 0;
    if (!mem_read((const uint8_t*)list + 0xC, &n, sizeof(n))) return nullptr;
    if (!mem_read((const uint8_t*)list + 0x10, &data, sizeof(data)) || !data) return nullptr;
    count = n;
    return list;
}

// --- poop -------------------------------------------------------------------------

bool piece_is_poop(void* piece) {
    void* meta = nullptr;
    if (!mem_read((const uint8_t*)piece + kFP_Meta, &meta, sizeof(meta)) || !meta) return false;
    char name[8];
    return mem_read_std_string((const uint8_t*)meta + kMeta_Name, name, sizeof(name)) &&
           strcmp(name, "poop") == 0;
}

bool entity_xy(void* comp, uintptr_t ent_off, double& x, double& y) {
    void* e = nullptr;
    if (!mem_read((const uint8_t*)comp + ent_off, &e, sizeof(e)) || !e) return false;
    double d[2];
    if (!mem_read((const uint8_t*)e + kEnt_X, d, sizeof(d))) return false;
    if (!std::isfinite(d[0]) || !std::isfinite(d[1])) return false;
    x = d[0]; y = d[1];
    return true;
}

bool poop_of(void* piece, Poop& p) {
    if (!piece || !piece_is_poop(piece)) return false;
    void* grid = nullptr;
    if (!mem_read((const uint8_t*)piece + kFP_Grid, &grid, sizeof(grid)) || !grid) return false;
    double px, py;
    if (!entity_xy(piece, kFP_Entity, px, py) || !entity_xy(grid, kFG_Entity, p.gx, p.gy))
        return false;
    p.piece = piece;
    p.cx = (int32_t)std::lround(px - p.gx);
    p.cy = (int32_t)std::lround(py - p.gy);
    return true;
}

bool same_grid(double ax, double ay, double bx, double by) {
    return std::fabs(ax - bx) < 0.5 && std::fabs(ay - by) < 0.5;
}

// Every poop in the house, now.
uint32_t read_poops(void* scene, Poop* out, uint32_t cap) {
    uint32_t n = 0, k = 0;
    void** data = nullptr;
    if (!list_of(scene, kType_FurnPiece, n, data)) return 0;
    for (uint32_t i = 0; i < n && k < cap; ++i) {
        void* piece = nullptr;
        if (!mem_read(data + i, &piece, sizeof(piece)) || !piece) continue;
        if (poop_of(piece, out[k])) ++k;
    }
    return k;
}

const Poop* poop_find(const Poop* set, uint32_t n, double gx, double gy, int32_t cx, int32_t cy) {
    for (uint32_t i = 0; i < n; ++i)
        if (set[i].cx == cx && set[i].cy == cy && same_grid(set[i].gx, set[i].gy, gx, gy))
            return &set[i];
    return nullptr;
}

void* grid_at(void* scene, double gx, double gy) {
    uint32_t n = 0;
    void** data = nullptr;
    if (!list_of(scene, kType_FurnGrid, n, data)) return nullptr;
    for (uint32_t i = 0; i < n; ++i) {
        void* grid = nullptr;
        double x, y;
        if (!mem_read(data + i, &grid, sizeof(grid)) || !grid) continue;
        if (entity_xy(grid, kFG_Entity, x, y) && same_grid(x, y, gx, gy)) return grid;
    }
    return nullptr;
}

// HOST: diff the house's poops against the last look and announce the new
// ones. A clean-up is announced by its hook, not here -- a vanished poop says
// nothing about who cleaned it, and the other peer may be the one who did.
void scan_poops(void* scene, ULONGLONG now) {
    if (now - g.poop_scan_at < kPoopScanMs) return;
    g.poop_scan_at = now;
    static Poop cur[kMaxPoops];
    const uint32_t n = read_poops(scene, cur, kMaxPoops);
    // The night makes the morning's poops and the client loads those with the
    // save, so a baseline is taken rather than diffed while it is pending.
    if (g.poop_based && !g.day_passed_at) {
        for (uint32_t i = 0; i < n; ++i) {
            if (poop_find(g.poops, g.npoops, cur[i].gx, cur[i].gy, cur[i].cx, cur[i].cy))
                continue;
            HousePoopMsg m{};
            m.op = kPoopSpawn;
            m.cx = cur[i].cx; m.cy = cur[i].cy;
            m.gx = cur[i].gx; m.gy = cur[i].gy;
            if (net_send_housepoop(m)) ++g.poop_sent;
            log_line_lvl(LogLevel::Trace, "HOUSE", "a poop appeared at cell %d,%d of the"
                         " grid at %.1f,%.1f -> sent", m.cx, m.cy, m.gx, m.gy);
        }
    }
    memcpy(g.poops, cur, sizeof(Poop) * n);
    g.npoops = n;
    g.poop_based = true;
}

void read_cats(void* scene) {
    g.ncats = 0;
    uint32_t n = 0; void** data = nullptr;
    if (!list_of(scene, kType_HouseCat, n, data)) return;
    for (uint32_t i = 0; i < n && g.ncats < kMaxHouseCats; ++i) {
        void* hc = nullptr;
        if (!mem_read(data + i, &hc, sizeof(hc)) || !hc) continue;
        Cat& c = g.cats[g.ncats];
        c.hc = hc;
        if (!mem_read((const uint8_t*)hc + kHC_Id, &c.id, sizeof(c.id)) || !c.id) continue;
        mem_read((const uint8_t*)hc + kHC_Anim, &c.anim, sizeof(c.anim));
        ++g.ncats;
    }
    g.nareas = 0;
    if (list_of(scene, kType_PlaceArea, n, data))
        for (uint32_t i = 0; i < n && g.nareas < kMaxAreas; ++i)
            mem_read(data + i, &g.areas[g.nareas++], sizeof(void*));
}

void* held_cat(void* scene) {
    uint32_t n = 0; void** data = nullptr;
    if (!list_of(scene, kType_ClickMgr, n, data) || !n) return nullptr;
    void* mgr = nullptr;
    if (!mem_read(data, &mgr, sizeof(mgr)) || !mgr) return nullptr;
    void* held = nullptr;
    mem_read((const uint8_t*)mgr + kCM_Held, &held, sizeof(held));
    return held;
}

const Cat* cat_by_hc(void* hc) {
    for (uint32_t i = 0; i < g.ncats; ++i) if (g.cats[i].hc == hc) return &g.cats[i];
    return nullptr;
}

uint8_t area_index(void* area) {
    if (!area) return 0xFF;
    for (uint32_t i = 0; i < g.nareas; ++i) if (g.areas[i] == area) return (uint8_t)i;
    return 0xFF;
}

void* entity_of(void* hc) {
    void* e = nullptr;
    mem_read((const uint8_t*)hc + kHC_Entity, &e, sizeof(e));
    return e;
}

bool read_state(const Cat& c, void* held, HouseCatState& st) {
    void* e = entity_of(c.hc);
    if (!e) return false;
    double d[3] = {}, flip = 1.0;
    if (!mem_read((const uint8_t*)e + kEnt_X, d, sizeof(d))) return false;
    mem_read((const uint8_t*)e + kEnt_Flip, &flip, sizeof(flip));
    st.id = c.id;
    st.x = (float)d[0]; st.y = (float)d[1]; st.z = (float)d[2];
    st.flip = (float)flip;
    mem_read((const uint8_t*)c.hc + kHC_Depth, &st.depth, sizeof(st.depth));
    int32_t face = 1, pose = 0;
    if (c.anim) {
        mem_read((const uint8_t*)c.anim + kAn_Face, &face, sizeof(face));
        mem_read((const uint8_t*)c.anim + kAn_Pose, &pose, sizeof(pose));
        if (!mem_read_std_string((const uint8_t*)c.anim + kAn_Name, st.anim, sizeof(st.anim)))
            st.anim[0] = 0;
    }
    st.face = (int8_t)(face < 0 ? -1 : 1);
    st.pose = (uint8_t)pose;
    void* area = nullptr;
    mem_read((const uint8_t*)c.hc + kHC_Area, &area, sizeof(area));
    st.area = area_index(area);
    st.held = (c.hc == held) ? 1 : 0;
    return std::isfinite(st.x) && std::isfinite(st.y) && std::isfinite(st.z);
}

void send_snapshot(void* held) {
    static HouseCatsMsg m;          // ~3.6 KB: not on the game's stack
    m.count = 0;
    const uint8_t me = self_peer();
    for (uint32_t i = 0; i < g.ncats && m.count < kMaxHouseCats; ++i) {
        if (authority_of(g.cats[i].id) != me) continue;
        if (read_state(g.cats[i], held, m.cats[m.count])) ++m.count;
    }
    if (!m.count) return;
    if (net_send_housecats(m)) ++g.sent;
}

// --- grabbing: authority follows the hand ----------------------------------------

void on_local_grab_change(void* held, ULONGLONG now) {
    const uint8_t me = self_peer();
    if (held == g.held_prev) return;
    // Let go of the previous one: it is still ours until it lands.
    if (g.held_prev && g.held_prev_id) {
        if (Auth* a = auth_find(g.held_prev_id))
            if (a->peer == me) { a->released = now; a->still = 0; }
    }
    g.held_prev = held;
    g.held_prev_id = 0;
    if (!held) return;
    const Cat* c = cat_by_hc(held);
    if (!c) return;
    g.held_prev_id = c->id;
    if (authority_of(c->id) == me) {
        if (Auth* a = auth_find(c->id)) { a->released = 0; a->still = 0; }  // caught again
        return;
    }
    // Take it. The host's word wins a tie: a client that grabs a cat the host
    // is already carrying keeps its own picture until the host lets go.
    if (me != kHostPeer && authority_of(c->id) != kHostPeer) return;
    auth_set(c->id, me);
    if (me == kHostPeer) {
        // The default is the host; tell the client it lost the cat.
    }
    HouseGrabMsg m{}; m.id = c->id; m.take = 1;
    net_send_housegrab(m);
    log_line_lvl(LogLevel::Trace, "HOUSE", "grabbed cat %016llx -- driving it until it lands",
                 (unsigned long long)c->id);
}

// A cat we threw: hand it back once it has come to rest.
void settle(void* held, ULONGLONG now) {
    const uint8_t me = self_peer();
    if (me == kHostPeer) return;                 // the host is the default anyway
    for (uint32_t i = 0; i < g.nauth; ++i) {
        Auth& a = g.auth[i];
        if (a.peer != me || !a.released) continue;
        const Cat* c = nullptr;
        for (uint32_t k = 0; k < g.ncats; ++k) if (g.cats[k].id == a.id) c = &g.cats[k];
        bool rest = false;
        if (c) {
            void* ph = nullptr;
            mem_read((const uint8_t*)c->hc + kHC_Phys, &ph, sizeof(ph));
            uint8_t ground = 0; double v[2] = {};
            if (ph) {
                mem_read((const uint8_t*)ph + kPh_Ground, &ground, 1);
                mem_read((const uint8_t*)ph + kPh_VX, v, sizeof(v));
            }
            rest = ground && std::fabs(v[0]) < 30.0 && std::fabs(v[1]) < 30.0;
        }
        if (rest) { if (!a.still) a.still = now; } else a.still = 0;
        const bool done = (!c) || (a.still && now - a.still > kSettleMs) ||
                          (now - a.released > kSettleMaxMs);
        if (!done) continue;
        send_snapshot(held);                      // the final resting place first
        HouseGrabMsg m{}; m.id = a.id; m.take = 0;
        net_send_housegrab(m);
        log_line_lvl(LogLevel::Trace, "HOUSE", "cat %016llx landed -- handed back to the host",
                     (unsigned long long)a.id);
        g.auth[i] = g.auth[--g.nauth];
        --i;
    }
}

} // namespace

void house_set_base(uintptr_t base) {
    g.base        = base;
    g.cats_ok     = false;
    g.scene_list  = (fn_scene_list)addr_of_call(C_SceneList);
    g.anim_set    = (fn_anim_set)  addr_of_call(C_AnimSetState);
    g.area_add    = (fn_area_op)   addr_of_call(C_AreaAdd);
    g.area_remove = (fn_area_op)   addr_of_call(C_AreaRemove);
    g.str_dtor    = (fn_str_dtor)  addr_of_call(C_GameStrDtor);
    g.cats_ok = g.scene_list && g.anim_set && g.area_add && g.area_remove && g.str_dtor;
    if (!g.cats_ok)
        log_line("HOUSE", "!! the house cat functions did not resolve -- each player will"
                          " see their own cats in the house");

    g.poop_spawn = (fn_this)addr_of_call(C_PoopSpawn);
    g.poop_clean = (fn_this)addr_of_call(C_PoopClean);
    g.end_day    = (fn_this)addr_of_call(C_HouseEndDay);
    g.poop_ok = g.cats_ok && g.poop_spawn && g.poop_clean;
    if (!g.poop_ok)
        log_line("HOUSE", "!! the poop functions did not resolve -- each player will have"
                          " to clean up their own");
    if (!g.end_day)
        log_line("HOUSE", "!! House::EndDay did not resolve -- the other player will not"
                          " see the night's screens, only the new day");

    g.resolved    = false;
    g.scene_cam   = (fn_scene_cam)  addr_of_call(C_SceneCamera);
    g.mouse_world = (fn_mouse_world)addr_of_call(C_CameraMouseWorld);
    g.viewproj    = (fn_viewproj)   addr_of_call(C_CameraViewProj);
    g.ndc         = (const float*)  addr_of_data(D_NdcRange);
    g.resolved = g.scene_cam && g.mouse_world && g.viewproj && g.ndc;
    if (!g.resolved)
        log_line("HOUSE", "!! the house camera functions did not resolve -- the other"
                          " player's pointer will not be shown in the house");
}

void house_on_update(void* house) {
    if (!house) return;
    g.tick = GetTickCount64();
    {
        if (!g.qpc_freq.QuadPart) QueryPerformanceFrequency(&g.qpc_freq);
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        double dt = g.qpc_last.QuadPart ? (double)(q.QuadPart - g.qpc_last.QuadPart) /
                                          (double)g.qpc_freq.QuadPart : 1.0 / 60.0;
        g.qpc_last = q;
        g.frame_dt = (dt > 0 && dt < 0.25) ? dt : 0.25;
    }

    void* scene = nullptr;
    if (!mem_read((const uint8_t*)house + kComp_Scene, &scene, sizeof(scene)) || !scene)
        return;

    // --- the camera, for the pointer -------------------------------------------
    if (void* cam = g.resolved ? g.scene_cam(scene) : nullptr) {
        // Both outputs are the game's own stack shapes; aligned the way its own
        // callers' locals are, in case either is ever read with an aligned load.
        alignas(16) double w[2] = {};
        g.mouse_world(cam, w);
        if (std::isfinite(w[0]) && std::isfinite(w[1])) {
            g.mx = w[0]; g.my = w[1]; g.have_mouse = true;
        }
        alignas(16) float m[16] = {};
        g.viewproj(cam, m);
        memcpy(g.vp, m, sizeof(g.vp));
        g.have_vp = true;

        if (!g.said_live) {
            g.said_live = true;
            log_line_lvl(LogLevel::Trace, "HOUSE", "house camera live: mouse at world"
                         " %.1f,%.1f", g.mx, g.my);
        }
    }

    // --- the cats -------------------------------------------------------------
    if (!g.cats_ok || !net_active()) return;
    g.house = house;
    read_cats(scene);
    if (!g.said_cats && g.ncats) {
        g.said_cats = true;
        log_line("HOUSE", "%u cat(s) and %u placement area(s) in the house -- the host"
                          " drives them, whoever picks one up drives that one",
                 g.ncats, g.nareas);
    }
    const ULONGLONG now = GetTickCount64();

    // --- the night: the client walks into the host's -----------------------
    if (g.night_go) {
        g.night_go = false;
        uint8_t passing = 0;
        mem_read((const uint8_t*)house + 0xB0, &passing, 1);
        if (passing) {
            g.night_seeded = false;
            log_line("HOUSE", "!! the host's night started while this house was already"
                              " ending its own day -- its screens may differ from the host's");
        } else {
            g.night_at = now;
            g.night_seen_inter = false;
            g.end_day(house);
            log_line("HOUSE", "the host ended the day -- playing the same night here,"
                              " from the host's dice");
        }
    }

    // --- poop: the host announces new ones -----------------------------------
    if (g.poop_ok && net_role() == NetRole::Host) scan_poops(scene, now);

    void* held = held_cat(scene);
    on_local_grab_change(held, now);
    settle(held, now);
    if (now - g.last_send >= kSendEveryMs) {
        g.last_send = now;
        send_snapshot(held);
    }
}

void house_on_cats(uint8_t from, const HouseCatsMsg& m) {
    if (!g.cats_ok) return;
    const ULONGLONG now = GetTickCount64();
    for (uint32_t i = 0; i < m.count; ++i) {
        const HouseCatState& st = m.cats[i];
        if (authority_of(st.id) != from) continue;   // stale word from a past holder
        CatTarget* t = tgt_find(st.id, true);
        if (!t) continue;
        t->st = st;
        t->at = now;
    }
    ++g.got;
}

void house_on_grab(uint8_t from, const HouseGrabMsg& m) {
    if (!g.cats_ok) return;
    if (m.take) {
        auth_set(m.id, from);
        // We may have been carrying it ourselves; the click manager keeps its
        // grip, but our picture now follows the other player's.
    } else if (authority_of(m.id) == from) {
        auth_set(m.id, kHostPeer);
    }
    if (CatTarget* t = tgt_find(m.id, false)) t->area_tried = 0xFE;
}

// The HouseCat::update detour asks this first. False = do not run the cat's
// own tick: the other player is driving it, so show their picture instead.
bool house_cat_update(void* hc) {
    if (!hc) return true;
    uint64_t id = 0;
    if (!mem_read((const uint8_t*)hc + kHC_Id, &id, sizeof(id)) || !id) return true;
    CatTarget* t = puppet_target(id);
    if (!t) return true;

    // Frame time for the smoother -- measured once a frame by the house tick,
    // NOT here: this runs once per cat, and the second cat would see a dt of
    // microseconds.
    const double dt = g.frame_dt;

    const HouseCatState& st = t->st;
    const double dx = st.x - t->sx, dy = st.y - t->sy;
    if (!t->placed || dx * dx + dy * dy > 600.0 * 600.0) {
        t->sx = st.x; t->sy = st.y; t->sz = st.z; t->placed = true;
    } else {
        const double k = 1.0 - std::exp(-dt / 0.05);
        t->sx += dx * k; t->sy += (st.y - t->sy) * k; t->sz += (st.z - t->sz) * k;
    }

    void* e = entity_of(hc);
    if (e) {
        const double pos[3] = { t->sx, t->sy, t->sz };
        mem_write((uint8_t*)e + kEnt_X, pos, sizeof(pos));
        const double flip = st.flip;
        mem_write((uint8_t*)e + kEnt_Flip, &flip, sizeof(flip));
        const double order = -(double)st.depth;
        mem_write((uint8_t*)e + kEnt_Order, &order, sizeof(order));
        mem_write((uint8_t*)e + kEnt_Order2, &order, sizeof(order));
    }
    mem_write((uint8_t*)hc + kHC_Depth, &st.depth, sizeof(st.depth));

    void* an = nullptr;
    mem_read((const uint8_t*)hc + kHC_Anim, &an, sizeof(an));
    if (an) {
        const int32_t face = st.face < 0 ? -1 : 1;
        mem_write((uint8_t*)an + kAn_Face, &face, sizeof(face));
        // The animation, only when it changed -- or, as a safety net, once a
        // second while it disagrees, in case something other than
        // sub_1409AA360 moved it.
        char cur[24] = {};
        int32_t cur_pose = 0;
        mem_read_std_string((const uint8_t*)an + kAn_Name, cur, sizeof(cur));
        mem_read((const uint8_t*)an + kAn_Pose, &cur_pose, sizeof(cur_pose));
        const bool differs = strcmp(cur, st.anim) != 0 || cur_pose != (int32_t)st.pose;
        const bool fresh = strcmp(t->injected, st.anim) != 0 || t->injected_pose != st.pose;
        const ULONGLONG now = GetTickCount64();
        const size_t len = strlen(st.anim);
        if (differs && len && len <= 15 && (fresh || now - t->injected_at > 1000)) {
            struct { char buf[16]; uint64_t size, cap; } s{};
            memcpy(s.buf, st.anim, len);
            s.size = len; s.cap = 15;
            g.anim_injecting = true;
            g.anim_set(an, (int)st.pose, &s);
            g.anim_injecting = false;
            strncpy_s(t->injected, st.anim, _TRUNCATE);
            t->injected_pose = st.pose;
            t->injected_at = now;
        }
    }

    // Which box or bed it is in. Tried once per change: the area's own accept
    // check can refuse, and asking again every frame would not change its mind.
    if (st.area != t->area_tried) {
        t->area_tried = st.area;
        void* cur = nullptr;
        mem_read((const uint8_t*)hc + kHC_Area, &cur, sizeof(cur));
        void* want = st.area < g.nareas ? g.areas[st.area] : nullptr;
        if (cur != want) {
            if (want) g.area_add(want, hc);
            else if (cur) {
                g.area_remove(cur, hc);
                void* none = nullptr;
                mem_write((uint8_t*)hc + kHC_Area, &none, sizeof(none));
            }
            ++g.areas_moved;
        }
    }
    ++g.puppeted;
    return false;
}

bool house_cat_late(void* hc) {
    if (!hc || !g.cats_ok || !net_active()) return true;
    uint64_t id = 0;
    if (!mem_read((const uint8_t*)hc + kHC_Id, &id, sizeof(id)) || !id) return true;
    return puppet_target(id) == nullptr;
}

bool house_cat_physics(void* phys) {
    if (!phys || !g.cats_ok || !net_active()) return true;
    // HouseCatPhysics+0x40 is its HouseCat (read off its own update).
    void* hc = nullptr;
    if (!mem_read((const uint8_t*)phys + 0x40, &hc, sizeof(hc)) || !hc) return true;
    uint64_t id = 0;
    if (!mem_read((const uint8_t*)hc + kHC_Id, &id, sizeof(id)) || !id) return true;
    if (!puppet_target(id)) return true;
    const double zero[2] = {};
    mem_write((uint8_t*)phys + kPh_VX, zero, sizeof(zero));
    return false;
}

bool house_anim_allowed(void* animator, void* str) {
    if (g.anim_injecting || !animator || !g.cats_ok || !net_active() || !house_active())
        return true;
    for (uint32_t i = 0; i < g.ncats; ++i) {
        if (g.cats[i].anim != animator) continue;
        if (!puppet_target(g.cats[i].id)) return true;
        // The callee would have freed the string; we did not call it, so we do.
        if (str) g.str_dtor(str);
        ++g.swallowed_anims;
        return false;
    }
    return true;
}

// The sign that ends the day -- or, with four cats in the box, departs. Its
// button runs House::init's lambda_1 (sub_1401F8EA0): `if (box full) depart()
// else EndDayTransition`. Identified by that callback's vtable, 0x140EFC860,
// read where House::init builds the button (0x1401F5BDE). Pinned to this build:
// on another one it simply never matches.
constexpr uintptr_t kRva_SignCallbackVt = 0x00EFC860;
constexpr uintptr_t kBtn_Callback       = 240;

bool house_on_button_click(void* button) {
    if (!button || !net_active() || net_role() != NetRole::Client) return true;
    void* impl = nullptr;
    uintptr_t vt = 0;
    if (!mem_read((const uint8_t*)button + kBtn_Callback, &impl, sizeof(impl)) || !impl)
        return true;
    if (!mem_read(impl, &vt, sizeof(vt))) return true;
    if (!g.base || vt != g.base + kRva_SignCallbackVt) return true;
    // The night is rolled once, in the host's house: ask the host to press its
    // sign. Whatever it does there -- end the day, or depart with a full box --
    // reaches this peer as a fresh save.
    g.sign_refused_at = GetTickCount64();
    PressMsg m{};
    m.node_seed = 0;                 // the house is no map node
    strncpy_s(m.name, "EndDay_Sign", _TRUNCATE);
    net_send_press(m);
    log_line("HOUSE", "asked the host to press the day's-end sign -- the night is rolled"
                      " once, in the host's house");
    return false;
}

bool house_sign_refused_recently() {
    return g.sign_refused_at && GetTickCount64() - g.sign_refused_at < 4000;
}

static void compare_night();

void house_on_day_passed(void*) {
    if (!net_active()) return;
    if (uint64_t* st = rng_global_stream()) {
        if (net_role() == NetRole::Host) {
            HouseNightMsg m{};
            m.phase = 1;
            memcpy(m.sim, st, sizeof(m.sim));
            memcpy(m.pres, st + 4, sizeof(m.pres));
            net_send_housenight(m);
        } else if (g.night_at) {
            memcpy(g.mine_after, st, sizeof(g.mine_after));
            g.mine_after_ok = true;
            compare_night();
        }
    }
    if (net_role() != NetRole::Host) return;
    g.day_passed_at = GetTickCount64();
    log_line("HOUSE", "the night has passed and the house is saved -- the new day goes to"
                      " the other player once the night's screens are done");
}

bool house_night_pending() { return g.day_passed_at != 0; }

// HOST: a run that started from the shared house. Departing goes through the
// act and class screens on the host alone; once the host stands on the map with
// the run loaded, its save is that run and the client is sent it fresh -- the
// same reload the end of the day uses.
static bool s_was_in_house = false;

void house_request_reload() {
    if (g.reload_at) return;
    g.reload_at   = GetTickCount64();
    g.escapes     = 0;
    g.last_escape = 0;
    // Quit To Menu is pressed by mgmp_leave the moment the pause menu is up;
    // the auto-Play and the save screen then load the host's new day, exactly
    // as they load a run.
    leave_request_local();
    log_line("HOUSE", "the host ended the day -- going out to the title screen to load"
                      " the new day");
}

bool house_reload_pending() { return g.reload_at != 0; }

bool house_pipe_done_allowed(void* impl) {
    // impl+8 is the HousePipe; its slot list is *(pipe+0x100), the cat its [0].
    void* pipe = nullptr;
    void** slots = nullptr;
    void* cat = nullptr;
    if (!mem_read((const uint8_t*)impl + 8, &pipe, sizeof(pipe)) || !pipe) return true;
    if (!mem_read((const uint8_t*)pipe + 0x100, &slots, sizeof(slots)) || !slots) return false;
    if (!mem_read(slots, &cat, sizeof(cat))) return false;
    if (cat) return true;
    log_line("HOUSE", "the stray had already left the pipe when its arrival finished --"
                      " skipped the drawer it would have opened on no cat");
    return false;
}

bool house_night_replaying() { return g.night_at != 0 || g.night_go; }

static void compare_night() {
    if (!g.host_after_ok || !g.mine_after_ok) return;
    g.host_after_ok = g.mine_after_ok = false;
    if (memcmp(g.host_after, g.mine_after, sizeof(g.host_after)) == 0)
        log_line_lvl(LogLevel::Trace, "HOUSE", "night AGREES with the host's: the same"
                     " dice were rolled the same number of times");
    else
        log_line_lvl(LogLevel::Warn, "HOUSE", "the night shown here DIFFERS from the host's"
                     " (stream %016llx vs %016llx) -- the two houses were not quite the"
                     " same; the new day loaded next is the host's either way",
                     (unsigned long long)g.mine_after[0], (unsigned long long)g.host_after[0]);
}

void house_before_pass_day(void*) {
    uint64_t* st = rng_global_stream();
    if (!st || !net_active()) return;
    if (net_role() == NetRole::Host) {
        HouseNightMsg m{};
        m.phase = 0;
        memcpy(m.sim, st, sizeof(m.sim));
        memcpy(m.pres, st + 4, sizeof(m.pres));
        net_send_housenight(m);
        return;
    }
    if (g.night_seeded) {
        memcpy(st, g.night_sim, sizeof(g.night_sim));
        memcpy(st + 4, g.night_pres, sizeof(g.night_pres));
        g.night_seeded = false;
        log_line_lvl(LogLevel::Trace, "HOUSE", "night seeded from the host's streams");
    }
}

void house_on_night(uint8_t, const HouseNightMsg& m) {
    if (net_role() != NetRole::Client) return;
    if (m.phase == 1) {
        memcpy(g.host_after, m.sim, sizeof(g.host_after));
        g.host_after_ok = true;
        compare_night();
        return;
    }
    if (!g.end_day || !house_active() || g.reload_at || house_night_replaying()) {
        log_line_lvl(LogLevel::Trace, "HOUSE", "the host's night began; not in the house"
                     " here, so only the new day will follow");
        return;
    }
    memcpy(g.night_sim, m.sim, sizeof(g.night_sim));
    memcpy(g.night_pres, m.pres, sizeof(g.night_pres));
    g.night_seeded = true;
    g.night_go = true;
    g.host_after_ok = g.mine_after_ok = false;
}

// --- poop ---------------------------------------------------------------------------

void house_on_poop(uint8_t from, const HousePoopMsg& m) {
    if (!g.poop_ok || !g.house || !house_active()) return;
    if (house_night_replaying() || g.reload_at) return;     // a new day is coming
    void* scene = nullptr;
    if (!mem_read((const uint8_t*)g.house + kComp_Scene, &scene, sizeof(scene)) || !scene)
        return;
    static Poop cur[kMaxPoops];
    const uint32_t n = read_poops(scene, cur, kMaxPoops);
    const Poop* have = poop_find(cur, n, m.gx, m.gy, m.cx, m.cy);
    ++g.poop_got;

    if (m.op == kPoopClean) {
        if (!have) {
            ++g.poop_missed;
            log_line_lvl(LogLevel::Trace, "HOUSE", "peer %u cleaned a poop at %d,%d that"
                         " is not here", (unsigned)from, m.cx, m.cy);
            return;
        }
        g.poop_injecting = true;
        g.poop_clean(have->piece);
        g.poop_injecting = false;
        // The host's next scan must not see it as anything at all.
        if (Poop* b = (Poop*)poop_find(g.poops, g.npoops, m.gx, m.gy, m.cx, m.cy))
            *b = g.poops[--g.npoops];
        return;
    }

    // A spawn: only ever from the host.
    if (net_role() != NetRole::Client || have) return;
    void* grid = grid_at(scene, m.gx, m.gy);
    if (!grid) {
        ++g.poop_missed;
        log_line("HOUSE", "!! the host's poop is on a grid at %.1f,%.1f this house does"
                          " not have", m.gx, m.gy);
        return;
    }
    g.force_cx = m.cx; g.force_cy = m.cy;
    g.force_cell = true;
    g.poop_injecting = true;
    g.poop_spawn(grid);
    g.poop_injecting = false;
    g.force_cell = false;
}

bool house_poop_clean(void* piece) {
    if (g.poop_injecting || !g.poop_ok || !net_active() || !house_active()) return true;
    Poop p;
    if (!poop_of(piece, p)) return true;
    HousePoopMsg m{};
    m.op = kPoopClean;
    m.cx = p.cx; m.cy = p.cy;
    m.gx = p.gx; m.gy = p.gy;
    if (net_send_housepoop(m)) ++g.poop_sent;
    if (Poop* b = (Poop*)poop_find(g.poops, g.npoops, p.gx, p.gy, p.cx, p.cy))
        *b = g.poops[--g.npoops];
    return true;
}

// The free cells of a grid, narrowed to one: the spawn then picks it whatever
// the dice say. Only while WE are spawning the host's poop.
void house_poop_cells(void*, void* out) {
    if (!g.force_cell || !out) return;
    g.force_cell = false;                     // the spawn asks exactly once
    uint32_t cap = 0;
    void* data = nullptr;
    if (!mem_read(out, &cap, sizeof(cap)) || !cap) return;
    if (!mem_read((const uint8_t*)out + 8, &data, sizeof(data)) || !data) return;
    const int32_t cell[2] = { g.force_cx, g.force_cy };
    const uint32_t one = 1;
    mem_write(data, cell, sizeof(cell));
    mem_write((uint8_t*)out + 4, &one, sizeof(one));
}

void house_pump() {
    const ULONGLONG now = GetTickCount64();

    // --- host: send the new day once the night is over ----------------------
    // Not straight from PassDay: it runs mid-fade with the night's interstitial
    // screens still to come, and the client reloading while the host is still
    // reading them is fine -- but the save must have been written, which it
    // has by then, and the house must be back, which it is once the
    // Interstitial scene is gone.
    if (g.day_passed_at && now - g.day_passed_at > 1500 &&
        !leave_scene_live("Interstitial")) {
        g.day_passed_at = 0;
        if (net_active()) savefile_publish_fresh("the day ended");
    }

    if (net_active() && net_role() == NetRole::Host) {
        if (house_active()) s_was_in_house = true;
        else if (s_was_in_house && follow_on_map() && savefile_adventure_is_loaded()) {
            s_was_in_house = false;
            savefile_publish_fresh("the host departed from the house");
        }
    }

    // --- client: our own copy of the host's night --------------------------
    // Wait it out before going anywhere: the screens are the whole point, and
    // the House scene may be gone while they are up.
    if (g.night_at) {
        const bool inter = leave_scene_live("Interstitial");
        if (inter) g.night_seen_inter = true;
        const ULONGLONG age = now - g.night_at;
        const bool over = (g.night_seen_inter && !inter && leave_scene_live("House")) ||
                          (!g.night_seen_inter && age > kNightNoInterMs) ||
                          age > kNightMaxMs;
        if (!over) return;
        g.night_at = 0;
        g.night_seeded = false;
        log_line_lvl(LogLevel::Trace, "HOUSE", "the night's screens are done here");
    }

    // --- client: get out to the title screen --------------------------------
    if (!g.reload_at) return;
    if (!leave_scene_live("House")) {          // out of the house: the rest is
        g.reload_at = 0;                        // the save screen's job
        return;
    }
    // Open the pause menu with Escape. There is no callable way in -- the menu
    // is built when a person pauses (CLAUDE.md, "The pause menu") -- so press
    // the key a person would. More than once: a first Escape may only close a
    // panel the player had open.
    if (now - g.last_escape < 1500) return;
    if (g.escapes >= 8) {
        if (g.escapes == 8) {
            ++g.escapes;
            log_line_lvl(LogLevel::Error, "HOUSE", "!! could not open the pause menu for"
                         " the player -- press Esc and Quit To Menu to load the new day");
        }
        return;
    }
    HWND hwnd = (HWND)ui_hwnd();
    if (!hwnd) return;
    g.last_escape = now;
    ++g.escapes;
    const LPARAM down = 1 | (0x01 << 16);
    const LPARAM up   = down | (1LL << 30) | (1LL << 31);
    PostMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, down);
    PostMessageW(hwnd, WM_KEYUP,   VK_ESCAPE, up);
}

void house_shutdown() {
    if (g.sent || g.got)
        log_line_lvl(LogLevel::Trace, "HOUSE", "done: %u snapshot(s) sent, %u received, %u"
                     " mirrored cat frame(s), %u local anim change(s) held back, %u area"
                     " move(s)", g.sent, g.got, g.puppeted, g.swallowed_anims, g.areas_moved);
    if (g.poop_sent || g.poop_got)
        log_line_lvl(LogLevel::Trace, "HOUSE", "poop: %u sent, %u received, %u not found",
                     g.poop_sent, g.poop_got, g.poop_missed);
    g.nauth = 0; g.ntgt = 0; g.ncats = 0; g.held_prev = nullptr;
    g.npoops = 0; g.poop_based = false;
}

bool house_active() {
    return g.tick && (GetTickCount64() - g.tick) < 250;
}

bool house_mouse_world(double& x, double& y) {
    if (!house_active() || !g.have_mouse) return false;
    x = g.mx; y = g.my;
    return true;
}

bool house_world_to_frac(double x, double y, float& nx, float& ny) {
    if (!house_active() || !g.have_vp || !g.ndc) return false;
    float r[4];
    if (!mem_read(g.ndc, r, sizeof(r))) return false;

    // Forward through VP, column-major, at z = 0 -- the exact inverse of what
    // sub_1409796D0 applies to the mouse.
    double ex = g.vp[0] * x + g.vp[4] * y + g.vp[12];
    double ey = g.vp[1] * x + g.vp[5] * y + g.vp[13];
    const double ew = g.vp[3] * x + g.vp[7] * y + g.vp[15];
    if (std::fabs(ew) > 1e-9) { ex /= ew; ey /= ew; }

    // sub_140986EE0: ndc_x = (1-tx)*r[0] + tx*r[1];  ndc_y = (1-ty)*r[3] + ty*r[2].
    const double dx = (double)r[1] - r[0];
    const double dy = (double)r[2] - r[3];
    if (std::fabs(dx) < 1e-9 || std::fabs(dy) < 1e-9) {
        if (!g.said_proj) {
            g.said_proj = true;
            log_line("HOUSE", "!! the NDC range reads %.3f %.3f %.3f %.3f -- cannot place"
                              " the other player's pointer", r[0], r[1], r[2], r[3]);
        }
        return false;
    }
    const double fx = (ex - r[0]) / dx;
    const double fy = (ey - r[3]) / dy;
    if (!std::isfinite(fx) || !std::isfinite(fy)) return false;
    nx = (float)fx;
    ny = (float)fy;
    return true;
}

} // namespace mgmp
