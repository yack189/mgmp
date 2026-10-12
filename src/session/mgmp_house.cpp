// mgmp_house.cpp -- see mgmp_house.h.

#include "mgmp_house.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_net.h"

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
    uintptr_t base = 0;
    bool said_cats = false;
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
    g.sign_refused_at = GetTickCount64();
    log_line("HOUSE", "the day's-end / depart sign is the host's for now -- this press was"
                      " ignored (each house would otherwise roll its own night)");
    return false;
}

bool house_sign_refused_recently() {
    return g.sign_refused_at && GetTickCount64() - g.sign_refused_at < 4000;
}

void house_shutdown() {
    if (g.sent || g.got)
        log_line_lvl(LogLevel::Trace, "HOUSE", "done: %u snapshot(s) sent, %u received, %u"
                     " mirrored cat frame(s), %u local anim change(s) held back, %u area"
                     " move(s)", g.sent, g.got, g.puppeted, g.swallowed_anims, g.areas_moved);
    g.nauth = 0; g.ntgt = 0; g.ncats = 0; g.held_prev = nullptr;
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
