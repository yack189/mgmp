// mgmp_party.cpp -- see mgmp_party.h.

#include "mgmp_party.h"

#include "mgmp_addresses.h"
#include "mgmp_resolve.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_follow.h"     // follow_on_map
#include "mgmp_leave.h"      // leave_in_run

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

typedef void* (__fastcall* fn_cat_by_id)(void* registry, uint64_t id);

// CatData+0x18 is the cat's name, a std::wstring (SSO capacity 7): the
// SerializeCatData version-19 path reads it with `cmp [rdx+18h], 7` and a
// `rax*2` length, i.e. UTF-16 code units. Read here only for the panel.
constexpr uintptr_t kCat_Name = 0x18;

// How far into a Character to look for its CatData pointer. Character is a
// large object (fields to past +0xEB4 are documented); a page is plenty and is
// still one guarded read per cat, once per battle.
constexpr uint32_t kCharScan = 0x1000;

constexpr uint32_t kMaxRun = 64;

struct State {
    bool on = false;
    bool is_host = false;

    fn_cat_by_id   by_id = nullptr;
    const void**   director_slot = nullptr;

    // THE table. On the host it is edited; on a client it is only ever
    // overwritten whole by an OWNERS from the host.
    uint32_t count = 0;
    uint64_t ids[kMaxOwned] = {};
    uint8_t  owner[kMaxOwned] = {};
    uint64_t sent_hash = 0;
    bool     dirty = false;

    // The panel's cached view of the run, refreshed from the game thread.
    PartyRow rows[kMaxRun];
    uint32_t row_count = 0;
    uint32_t tick = 0;

    // The Character -> CatData offset found at the last battle, 0 = unknown.
    uint32_t link_off = 0;

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

bool safe_by_id(void* registry, uint64_t id, void** out) {
    __try { *out = g.by_id(registry, id); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Run {
    void*           registry = nullptr;
    uint32_t        count = 0;
    uint64_t        ids[kMaxRun] = {};
    void*           cats[kMaxRun] = {};
};

// The run's cat ids in order, and each one's CatData. False outside a run.
bool read_run(Run& r) {
    if (!g.director_slot || !g.by_id) return false;
    const uint8_t* dir = nullptr;
    if (!mem_read(g.director_slot, &dir, sizeof(dir)) || !dir) return false;
    const uint64_t* ids = nullptr;
    if (!mem_read(dir + kDir_CatRegistry, &r.registry, sizeof(r.registry)) || !r.registry) return false;
    if (!mem_read(dir + kDir_CatIdCount, &r.count, sizeof(r.count))) return false;
    if (!mem_read(dir + kDir_CatIdData, &ids, sizeof(ids)) || !ids) return false;
    if (r.count == 0 || r.count > kMaxRun) return false;
    for (uint32_t i = 0; i < r.count; ++i) {
        if (!mem_read(&ids[i], &r.ids[i], sizeof(uint64_t)) || !r.ids[i]) return false;
        if (!safe_by_id(r.registry, r.ids[i], &r.cats[i]) || !r.cats[i]) return false;
    }
    return true;
}

void read_name(const void* cat, char* out, size_t cap) {
    out[0] = 0;
    const uint8_t* s = (const uint8_t*)cat + kCat_Name;
    uint64_t size = 0, capw = 0;
    if (!mem_read(s + 0x10, &size, sizeof(size)) || !mem_read(s + 0x18, &capw, sizeof(capw)))
        return;
    if (size == 0 || size > 64 || capw < size) return;
    const wchar_t* chars = (const wchar_t*)s;
    if (capw > 7 && !mem_read(s, &chars, sizeof(chars))) return;
    wchar_t buf[65] = {};
    if (!mem_read(chars, buf, (size_t)size * sizeof(wchar_t))) return;
    buf[size] = 0;
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, (int)cap, nullptr, nullptr);
}

int find(uint64_t id) {
    for (uint32_t i = 0; i < g.count; ++i) if (g.ids[i] == id) return (int)i;
    return -1;
}

uint64_t table_hash() {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < g.count; ++i) {
        const uint8_t* b = (const uint8_t*)&g.ids[i];
        for (int k = 0; k < 8; ++k) { h ^= b[k]; h *= 1099511628211ULL; }
        h ^= g.owner[i]; h *= 1099511628211ULL;
    }
    return h ^ g.count;
}

// The peer that is not the host, for defaults. kHostPeer if nobody else is here.
uint8_t first_client() {
    uint8_t ids[kMaxPeers] = {};
    const uint8_t n = net_peer_count();
    if (!net_peer_ids(ids, kMaxPeers)) return kHostPeer;
    for (uint8_t i = 0; i < n && i < kMaxPeers; ++i)
        if (ids[i] != kHostPeer) return ids[i];
    return kHostPeer;
}

bool member(uint8_t peer) {
    if (peer == kHostPeer) return true;
    uint8_t ids[kMaxPeers] = {};
    const uint8_t n = net_peer_count();
    if (!net_peer_ids(ids, kMaxPeers)) return false;
    for (uint8_t i = 0; i < n && i < kMaxPeers; ++i) if (ids[i] == peer) return true;
    return false;
}

void set_owner(uint64_t id, uint8_t peer) {
    int i = find(id);
    if (i < 0) {
        if (g.count >= kMaxOwned) {
            // Drop the oldest entry: tables only grow as cats join runs, and a
            // cat that has not been seen for 64 newer ones is not coming back
            // into a battle this session.
            memmove(&g.ids[0], &g.ids[1], (kMaxOwned - 1) * sizeof(uint64_t));
            memmove(&g.owner[0], &g.owner[1], kMaxOwned - 1);
            g.count = kMaxOwned - 1;
        }
        i = (int)g.count++;
        g.ids[i] = id;
    }
    if (g.owner[i] != peer || i == (int)g.count - 1) g.dirty = true;
    g.owner[i] = peer;
}

void send_table(const char* why, bool force) {
    if (!g.is_host || !net_active()) return;
    const uint64_t h = table_hash();
    if (!force && h == g.sent_hash) return;
    OwnersMsg m{};
    m.count = g.count;
    memcpy(m.ids, g.ids, g.count * sizeof(uint64_t));
    memcpy(m.owner, g.owner, g.count);
    if (net_send_owners(m)) {
        g.sent_hash = h;
        log_line("PARTY", "-> owners: %u cat(s) (%s)", g.count, why);
    }
}

// HOST: give every run cat the table does not know yet an owner -- the first
// half (rounded up) to the host, the rest to the other player. Exactly the old
// automatic split, so a session where nobody touches the panel plays as before.
void assign_defaults(const Run& r) {
    uint32_t unknown = 0;
    for (uint32_t i = 0; i < r.count; ++i) if (find(r.ids[i]) < 0) ++unknown;
    if (!unknown) return;
    const uint8_t other = first_client();
    const uint32_t to_host = (r.count + 1) / 2;
    for (uint32_t i = 0; i < r.count; ++i) {
        if (find(r.ids[i]) >= 0) continue;
        set_owner(r.ids[i], i < to_host ? kHostPeer : other);
    }
}

} // namespace

// ---------------------------------------------------------------------------

void party_set_base(uintptr_t /*base*/) {
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }
    g.director_slot = (const void**)addr_of_data(D_MewDirectorPtr);
    g.by_id = (fn_cat_by_id)addr_of_call(C_CatDataById);
    if (!g.by_id || !g.director_slot)
        log_line("PARTY", "!! CatData::by_id or the director did not resolve -- cats "
                          "will be split by roster order");
}

void party_init() {
    Guard guard;
    g.on = (net_role() != NetRole::None);
    g.is_host = (net_role() == NetRole::Host);
    g.sent_hash = 0;
    g.link_off = 0;
    if (!g.is_host) { g.count = 0; }   // a client's table is whatever the host sends
}

void party_shutdown() {
    Guard guard;
    g.on = false;
    g.row_count = 0;
}

void party_pump() {
    if (!g.on) return;
    if ((g.tick++ % 20) != 0) return;   // a few times a second is plenty

    Run r;
    const bool have = read_run(r);
    Guard guard;
    if (!have) { g.row_count = 0; return; }

    if (g.is_host) {
        assign_defaults(r);
        if (g.dirty) { g.dirty = false; send_table("changed", false); }
    }

    g.row_count = 0;
    for (uint32_t i = 0; i < r.count && g.row_count < kMaxRun; ++i) {
        PartyRow& row = g.rows[g.row_count++];
        row.id = r.ids[i];
        read_name(r.cats[i], row.name, sizeof(row.name));
        if (!row.name[0]) _snprintf_s(row.name, sizeof(row.name), _TRUNCATE, "Gato %u", i + 1);
        const int k = find(r.ids[i]);
        row.owner = k >= 0 ? g.owner[k] : kNoPeer;
    }
}

void party_publish(const char* why) {
    if (!g.on || !g.is_host) return;
    Run r;
    const bool have = read_run(r);
    Guard guard;
    if (have) assign_defaults(r);
    g.dirty = false;
    send_table(why, true);
}

void party_on_owners(const OwnersMsg& m) {
    if (!g.on || g.is_host) return;
    Guard guard;
    g.count = m.count < kMaxOwned ? m.count : kMaxOwned;
    memcpy(g.ids, m.ids, g.count * sizeof(uint64_t));
    memcpy(g.owner, m.owner, g.count);
    log_line_lvl(LogLevel::Trace, "PARTY", "<- owners: %u cat(s)", g.count);
}

void party_on_claim(uint8_t from, const ClaimMsg& m) {
    if (!g.on || !g.is_host) return;
    if (!party_editable()) {
        log_line("PARTY", "!! peer %u asked for cat %016llx outside the map -- ignored, "
                          "the next battle has already been split", (unsigned)from,
                 (unsigned long long)m.id);
        return;
    }
    if (!member(m.owner)) return;
    Guard guard;
    set_owner(m.id, m.owner);
    g.dirty = false;
    send_table("a player changed the party", true);
}

void party_request(uint64_t id, uint8_t owner) {
    if (!g.on || !party_editable()) return;
    if (g.is_host) {
        Guard guard;
        set_owner(id, owner);
        g.dirty = false;
        send_table("the host changed the party", true);
    } else {
        ClaimMsg m{};
        m.id = id; m.owner = owner;
        if (net_send_claim(m))
            log_line("PARTY", "-> asked the host to give cat %016llx to peer %u",
                     (unsigned long long)id, (unsigned)owner);
    }
}

uint8_t party_owner(uint64_t id) {
    Guard guard;
    const int i = find(id);
    return i >= 0 ? g.owner[i] : kNoPeer;
}

uint64_t party_catdata_id(const void* cat) {
    uint64_t id = 0;
    if (!cat || !mem_read(cat, &id, sizeof(id))) return 0;
    return id;
}

uint64_t party_find_run_cat(const void* obj, uint32_t scan) {
    if (!obj || !scan || scan > 0x2000) return 0;
    Run r;
    if (!read_run(r)) return 0;
    static uint8_t buf[0x2000];
    if (!mem_read(obj, buf, scan)) return 0;
    int hit = -1;
    for (uint32_t off = 0; off + 8 <= scan; off += 8) {
        const void* p = *(const void* const*)(buf + off);
        for (uint32_t j = 0; j < r.count; ++j) {
            if (r.cats[j] != p) continue;
            if (hit >= 0 && hit != (int)j) return 0;   // two cats -- ambiguous
            hit = (int)j;
        }
    }
    return hit >= 0 ? r.ids[hit] : 0;
}

bool party_owns_catdata(const void* cat) {
    const uint64_t id = party_catdata_id(cat);
    if (!id) return false;
    return party_owner(id) == net_self();
}

bool party_split(const void* const* chars, uint32_t humans, bool* local_out) {
    if (!g.on || !humans || !chars || !local_out) return false;
    Run r;
    if (!read_run(r)) {
        log_line("PARTY", "!! could not read the run's cats -- automatic split");
        return false;
    }

    // Which run cat is each human Character? First by a pointer the Character
    // holds to its CatData, found by scanning and accepted only if it links
    // every human to a distinct run cat at one common offset.
    int which[kMaxRun];
    for (uint32_t k = 0; k < humans && k < kMaxRun; ++k) which[k] = -1;
    bool linked = false;

    static uint8_t page[kMaxRun][kCharScan];
    bool readable = humans <= kMaxRun;
    for (uint32_t k = 0; readable && k < humans; ++k)
        readable = chars[k] && mem_read(chars[k], page[k], kCharScan);

    if (readable) {
        // Every run cat must be found, each in a different human; a human
        // that points at none is not a run cat (measured 2026-10-13: a fifth
        // PlayerBrain unit, spawned after the enemies, in a battle with four
        // run cats -- requiring all five to link threw the whole party table
        // away and split the battle by roster order).
        auto try_off = [&](uint32_t off) -> bool {
            bool used[kMaxRun] = {};
            uint32_t found = 0;
            for (uint32_t k = 0; k < humans; ++k) {
                const void* p = *(const void* const*)(page[k] + off);
                which[k] = -1;
                if (!p) continue;
                for (uint32_t j = 0; j < r.count; ++j) {
                    if (r.cats[j] != p) continue;
                    if (used[j]) return false;          // two humans, one cat
                    used[j] = true;
                    which[k] = (int)j;
                    ++found;
                    break;
                }
            }
            return found == r.count && found > 0;
        };
        if (g.link_off && try_off(g.link_off)) linked = true;
        for (uint32_t off = 0; !linked && off + 8 <= kCharScan; off += 8)
            if (try_off(off)) { linked = true; g.link_off = off; }
        if (linked)
            log_line_lvl(LogLevel::Trace, "PARTY", "Character -> CatData at +0x%X", g.link_off);
    }

    if (!linked) {
        if (r.count != humans) {
            log_line("PARTY", "!! %u human cat(s) in battle but %u in the run, and no "
                              "Character->CatData link found -- automatic split",
                     humans, r.count);
            return false;
        }
        for (uint32_t k = 0; k < humans; ++k) which[k] = (int)k;
        log_line("PARTY", "no Character->CatData link found -- matching the battle's "
                          "cats to the run in order");
    }

    Guard guard;
    const uint8_t self = net_self();
    for (uint32_t k = 0; k < humans; ++k) {
        if (which[k] < 0) {
            // Player-controlled but not one of the run's cats: the host's.
            local_out[k] = (self == kHostPeer);
            log_line("PARTY", "battle cat %u is player-controlled but not one of the run's"
                              " cats -> the host plays it%s", k,
                     local_out[k] ? " (this peer)" : "");
            continue;
        }
        const uint64_t id = r.ids[which[k]];
        const int i = find(id);
        if (i < 0) {
            log_line("PARTY", "!! cat %016llx has no owner in the table -- automatic split",
                     (unsigned long long)id);
            return false;
        }
        // A cat whose owner has left the session is the host's to play.
        const uint8_t who = member(g.owner[i]) ? g.owner[i] : kHostPeer;
        local_out[k] = (who == self);
        char nm[48];
        read_name(r.cats[which[k]], nm, sizeof(nm));
        log_line("PARTY", "battle cat %u = '%s' (%016llx) -> peer %u%s", k,
                 nm[0] ? nm : "?", (unsigned long long)id, (unsigned)who,
                 local_out[k] ? " (this peer)" : "");
    }
    return true;
}

uint32_t party_rows(PartyRow* out, uint32_t cap) {
    Guard guard;
    const uint32_t n = g.row_count < cap ? g.row_count : cap;
    for (uint32_t i = 0; i < n; ++i) out[i] = g.rows[i];
    return n;
}

bool party_editable() {
    return g.on && follow_on_map();
}

} // namespace mgmp
