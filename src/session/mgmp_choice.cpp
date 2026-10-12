// mgmp_choice -- see mgmp_choice.h for why replicating the CHOICE replaces
// replicating everything the choice does.
#include "mgmp_choice.h"
#include "mgmp_follow.h"
#include "mgmp_party.h"
#include "mgmp_net.h"
#include "mgmp_config.h"
#include "mgmp_tuning.h"
#include "mgmp_mem.h"
#include "mgmp_log.h"
#include "mgmp_addresses.h"
#include "mgmp_resolve.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// --- the two option arrays -------------------------------------------------
//
// Both are {T* begin, T* end} with sizeof(T) == 240, and both are built in
// authored file order. Read off setupActionChoice__inner_0 (v20 = *(v1+28),
// v161 = *(v1+29), then "v20 += 240") and off the level-up button callback,
// which resolves *(LevelUpScreen+864) + 240 * index.
constexpr uintptr_t kEvt_OptBegin  = 224;
constexpr uintptr_t kEvt_OptEnd    = 232;

constexpr uintptr_t kLvl_OptBegin  = 864;
constexpr uintptr_t kLvl_OptEnd    = 872;
constexpr uintptr_t kLvl_Committed = 792;   // select_option's "already chose" guard
constexpr uintptr_t kLvl_Subject   = 160;   // the CatData* the screen levels up

constexpr uintptr_t kOptStride     = 240;

// The cat an event is about: WorldEvent+296. The effect handlers spin calls
// read it as `this->+0x128` and pass it straight to CatData methods
// (e.g. 0x14091A7A0 -> sub_1400B8A80), and self_status_next_fight writes
// through it. Validated at run time rather than trusted: the pointer has to
// resolve to a cat id the party table knows, or the host decides.
constexpr uintptr_t kEvt_Subject   = 0x128;

// MapNodeType values whose screen is a chest rather than a decision: treasure,
// furniturebox, foodbox. The host alone opens those.
bool chest_node(uint32_t t) { return t >= 13 && t <= 15; }

// An event option entry's stat key -- "str".."lck", "coins", "quest", "none",
// "home". A std::string at entry+64, compared against exactly those literals
// all the way through setupActionChoice.
constexpr uintptr_t kEvtOpt_Stat = 64;
// A LevelUpOption's type (1 stats, 2/3 pools, 4/7 ability, 5, 6 item) and its
// authored name, both read straight off select_option's own switch.
constexpr uintptr_t kLvlOpt_Type = 0;
constexpr uintptr_t kLvlOpt_Name = 200;

// The shipped UI pads out to 4 buttons per screen. This bound only has to be
// loose enough not to reject a legitimate list and tight enough to refuse a
// wild pointer.
constexpr uint32_t kMaxOptions = 64;

using fn_cap = void (__fastcall*)(void*);

struct State {
    bool on        = false;
    bool is_client = false;

    // Resolved from the module base, prologue-checked, exactly like catsync's
    // call targets: a bad address here would run an unrelated function with our
    // arguments on the game's stack.
    uintptr_t base     = 0;
    bool      resolved = false;
    fn_cap    evt_commit = nullptr;   // sub_140937F30
    fn_cap    lvl_click  = nullptr;   // sub_140386810

    void* world_event  = nullptr;   // latest WorldEvent*,    from its update
    void* level_screen = nullptr;   // latest LevelUpScreen*, from its update

    // A CHOICE can arrive before this peer's screen exists -- the client is
    // following the host into the node and may not have opened it yet. Hold it
    // rather than dropping it, the way mgmp_follow holds a node.
    bool     pending[2]          = {false, false};
    // The node the host was standing in when it made the choice, and the node
    // this peer is standing in now. A held choice may only be applied when the
    // two agree -- the single check that stops a choice outliving its node.
    uint64_t pending_seed[2]     = {0, 0};
    uint64_t here_seed           = 0;

    // NODES THIS PEER WILL NEVER ENTER, so a choice that arrives for one after
    // the fact can be refused instead of held forever.
    //
    // The order is not fixed: mgmp_follow can discard a node before the host
    // has even made its choice there, in which case the CHOICE arrives second
    // and there is nothing held for choice_on_node_skipped to drop. Without
    // this ring that message would sit in the pending slot indefinitely and
    // block the next real one.
    //
    // Same device, and the same 16 entries, as the retired-battle_id ring in
    // mgmp_battleid.h -- and for the same reason: an id that falls off the end
    // degrades to "unknown, hold it", never to a wrong apply.
    static constexpr uint32_t kSkippedRing = 16;
    uint64_t skipped[kSkippedRing] = {};
    uint32_t skipped_next          = 0;
    uint32_t pending_index[2]    = {0, 0};
    uint32_t pending_count[2]    = {0, 0};
    uint32_t pending_aux[2]      = {0, 0};
    char     pending_name[2][48] = {};
    uint64_t pending_subject[2]  = {0, 0};   // level-up: which cat it is for

    // Level-up picks that arrived while another was still held. After a fight
    // the owner can click through two cats' screens faster than this peer
    // shows them, and the single slot above used to drop the older pick.
    static constexpr uint32_t kLvlQueue = 8;
    ChoiceMsg lvl_queue[kLvlQueue];
    uint32_t  lvl_queued = 0;
    bool     pending_warned[2]   = {false, false};

    // Set while we drive the game's own commit path, so the hook that exists to
    // SWALLOW a click does not swallow the click we just injected. Not
    // thread-local: every path that touches it is the game thread.
    bool injecting = false;

    uint32_t sent      = 0;
    uint32_t applied   = 0;
    uint32_t swallowed = 0;

    // The event screen's liveness.
    ULONGLONG event_tick = 0;
    void    (*button_click)(void*, bool) = nullptr;

    // WHO ANSWERS THIS EVENT: the player who owns the cat it is about, cached
    // per WorldEvent so the log says it once.
    void*    ctl_we   = nullptr;
    uint8_t  ctl_peer = kHostPeer;
    uint64_t ctl_cat  = 0;

    // BUTTONS THE OTHER PLAYER PRESSED, to press here in the same order. An
    // event's continue, a chest's items and its exit. Each names the node it
    // was pressed on, so one can never land on a later screen.
    struct Press {
        uint64_t  seed  = 0;
        char      name[48] = {};
        ULONGLONG due   = 0;      // first tick it was due here (0 = not yet)
        uint32_t  tries = 0;
    };
    static constexpr uint32_t kPresses = 16;
    Press     pq[kPresses];
    uint32_t  pq_n          = 0;
    ULONGLONG press_last    = 0;
    bool      press_injecting = false;
    uint32_t  presses_sent  = 0;
    uint32_t  presses_done  = 0;
    uint32_t  presses_eaten = 0;
    // The node on which the other player has already pressed the event's
    // continue. From then on this player's own continue is let through too, so
    // a repeat that fails can never leave anyone stuck on a dead screen.
    uint64_t  exit_released = 0;

    CRITICAL_SECTION cs;
    bool cs_ready = false;
};

State g;

struct Guard {
    Guard()  { if (g.cs_ready) EnterCriticalSection(&g.cs); }
    ~Guard() { if (g.cs_ready) LeaveCriticalSection(&g.cs); }
};

const char* kind_name(uint8_t k) { return k == kChoiceLevelUp ? "level-up" : "event"; }

// Both called with the Guard held.
void remember_skipped(uint64_t seed) {
    if (!seed) return;
    for (uint32_t i = 0; i < State::kSkippedRing; ++i)
        if (g.skipped[i] == seed) return;          // already known, keep it once
    g.skipped[g.skipped_next] = seed;
    g.skipped_next = (g.skipped_next + 1) % State::kSkippedRing;
}

bool was_skipped(uint64_t seed) {
    if (!seed) return false;
    for (uint32_t i = 0; i < State::kSkippedRing; ++i)
        if (g.skipped[i] == seed) return true;
    return false;
}

// --- who controls the screen ---------------------------------------------------

bool is_exit(const char* n)  { return strcmp(n, "Event_ExitButton") == 0; }
bool is_chest(const char* n) { return strncmp(n, "ChestButton_", 12) == 0; }

// A press may only ever be repeated for one of these. A PRESS naming anything
// else is refused: the other peer does not get to press arbitrary buttons here.
bool mirrorable(const char* n) {
    return is_exit(n) || is_chest(n) || strcmp(n, "Shop_ExitButton") == 0;
}

uint8_t self_peer() {
    const uint8_t me = net_self();
    if (me != kNoPeer) return me;
    return g.is_client ? kNoPeer : kHostPeer;
}

// Called with the Guard held.
uint8_t event_controller(void* we) {
    if (!we) return kHostPeer;
    // Without both call targets this peer cannot apply the other's answer, so
    // it must not hand the event away: the old rule, the host answers.
    if (!g.resolved) return kHostPeer;
    if (chest_node(follow_here_type())) return kHostPeer;
    if (we == g.ctl_we) return g.ctl_peer;

    const void* cat = nullptr;
    uint64_t id = 0;
    if (mem_read((const uint8_t*)we + kEvt_Subject, &cat, sizeof(cat)) && cat)
        id = party_catdata_id(cat);
    uint8_t who = id ? party_owner(id) : kNoPeer;
    if (who == kNoPeer) who = kHostPeer;

    g.ctl_we   = we;
    g.ctl_peer = who;
    g.ctl_cat  = (id && party_owner(id) != kNoPeer) ? id : 0;
    log_line("CHOICE", "this event is about cat %016llx -- %s answers it",
             (unsigned long long)g.ctl_cat,
             who == self_peer() ? "THIS player" : "the other player");
    return who;
}

bool event_mine(void* we) { return event_controller(we) == self_peer(); }

// Called with the Guard held.
void press_pop() {
    if (!g.pq_n) return;
    memmove(&g.pq[0], &g.pq[1], (g.pq_n - 1) * sizeof(State::Press));
    --g.pq_n;
}

void press_push(uint64_t seed, const char* name, const char* why) {
    for (uint32_t i = 0; i < g.pq_n; ++i)
        if (g.pq[i].seed == seed && strcmp(g.pq[i].name, name) == 0 && is_exit(name))
            return;                                   // one continue is enough
    if (g.pq_n >= State::kPresses) {
        log_line("CHOICE", "!! %u button presses already waiting -- dropping '%s'",
                 g.pq_n, name);
        return;
    }
    State::Press& p = g.pq[g.pq_n++];
    p = State::Press{};
    p.seed = seed;
    strncpy_s(p.name, name, _TRUNCATE);
    log_line("CHOICE", "will press '%s' here (%s)", name, why);
}

// --- reading a std::string out of the game ---------------------------------
//
// MSVC's std::string is {union{char buf[16]; char* ptr;}, size, capacity}, so
// the characters are inline when capacity <= 15 and behind a pointer otherwise.
// Copy at most cap-1 and always terminate: a garbled length must not walk.
bool read_game_string(const void* str, char* out, uint32_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!str) return false;
    uint64_t len = 0, capacity = 0;
    if (!mem_read((const uint8_t*)str + 16, &len, sizeof(len))) return false;
    if (!mem_read((const uint8_t*)str + 24, &capacity, sizeof(capacity))) return false;
    if (len > 4096) return false;                  // not a string we recognise
    const uint8_t* chars = (const uint8_t*)str;
    if (capacity > 15) {
        void* p = nullptr;
        if (!mem_read((const uint8_t*)str, &p, sizeof(p)) || !p) return false;
        chars = (const uint8_t*)p;
    }
    uint32_t n = (uint32_t)len;
    if (n > cap - 1) n = cap - 1;
    if (n && !mem_read(chars, out, n)) { out[0] = '\0'; return false; }
    out[n] = '\0';
    return true;
}

// --- the option arrays -----------------------------------------------------

bool read_options(const void* screen, uintptr_t begin_off, uintptr_t end_off,
                  uint8_t*& begin, uint32_t& count) {
    begin = nullptr; count = 0;
    if (!screen) return false;
    uint8_t* b = nullptr; uint8_t* e = nullptr;
    if (!mem_read((const uint8_t*)screen + begin_off, &b, sizeof(b))) return false;
    if (!mem_read((const uint8_t*)screen + end_off,   &e, sizeof(e))) return false;
    if (!b || e < b) return false;
    const uintptr_t bytes = (uintptr_t)(e - b);
    if (bytes % kOptStride) return false;           // not the array we think it is
    const uintptr_t n = bytes / kOptStride;
    if (n == 0 || n > kMaxOptions) return false;
    begin = b; count = (uint32_t)n;
    return true;
}

void option_name(uint8_t kind, const uint8_t* entry, char* out, uint32_t cap) {
    const uintptr_t off = (kind == kChoiceLevelUp) ? kLvlOpt_Name : kEvtOpt_Stat;
    if (!read_game_string(entry + off, out, cap)) snprintf(out, cap, "?");
}

uint32_t option_type(const uint8_t* entry) {
    uint32_t t = 0;
    mem_read(entry + kLvlOpt_Type, &t, sizeof(t));
    return t;
}

// --- injecting -------------------------------------------------------------
//
// Both are the game's own commit path with our index substituted, rather than a
// reimplementation of what the commit does. That matters: sub_140937F30 also
// sets MewDirector+1812 and calls sub_14091AA00, and the level-up callback also
// copy-constructs the LevelUpOption through sub_14037BBB0. Neither is something
// worth reproducing by hand.

bool inject_event(uint32_t index) {
    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(g.world_event, kEvt_OptBegin, kEvt_OptEnd, begin, count)) return false;
    if (index >= count) return false;

    // sub_140937F30 reads only a1+8 and a1+16 and never touches the vftable
    // slot, so a bare three-qword block is a complete capture for it.
    void* cap[3] = { nullptr, g.world_event, begin + (uintptr_t)index * kOptStride };
    if (!g.evt_commit) return false;
    // Calling the spliced address re-enters our own hook, which sees `injecting`
    // and answers "run the original" -- so the game's commit runs exactly once
    // and we do not need the trampoline exposed outside mgmp_hooks.cpp.
    g.injecting = true;
    g.evt_commit(cap);
    g.injecting = false;
    return true;
}

bool inject_level(uint32_t index) {
    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(g.level_screen, kLvl_OptBegin, kLvl_OptEnd, begin, count)) return false;
    if (index >= count) return false;

    // sub_140386810's capture is {vftable, LevelUpScreen*, int index}; it reads
    // the index as a 32-bit signed at +16 and does the +240*index itself.
    struct Cap { void* vt; void* screen; int32_t index; int32_t pad; };
    Cap cap = { nullptr, g.level_screen, (int32_t)index, 0 };
    if (!g.lvl_click) return false;
    g.injecting = true;
    g.lvl_click(&cap);
    g.injecting = false;
    return true;
}

// Shared by both apply ticks.
// The cat a level-up screen is for, by CatData id. 0 if unreadable.
//
// LevelUpScreen+160 was documented as the subject CatData* on an older build;
// on 4127cd6a its first word read as 2ad055323c59f0ca, not a cat id (measured
// 2026-10-12), so the owner check silently fell back to "the host picks". The
// screen is searched for a pointer to one of the run's cats instead -- the same
// discovery that links a battle Character to its CatData -- and the documented
// offset is only a second opinion.
uint64_t level_subject(const void* screen) {
    if (!screen) return 0;
    if (const uint64_t id = party_find_run_cat(screen, 0x400)) return id;
    const void* cat = nullptr;
    if (!mem_read((const uint8_t*)screen + kLvl_Subject, &cat, sizeof(cat)) || !cat)
        return 0;
    const uint64_t id = party_catdata_id(cat);
    return (id && party_owner(id) != kNoPeer) ? id : 0;
}

// Does THIS peer pick this level-up? The cat's owner does (mgmp_party). A cat
// the table does not know falls back to the old rule -- the host picks.
bool level_is_mine(const void* screen) {
    const uint64_t id = level_subject(screen);
    const uint8_t who = id ? party_owner(id) : kNoPeer;
    if (who == kNoPeer) return !g.is_client;
    return who == net_self();
}

void apply_pending(uint8_t kind, void* screen) {
    if (!g.pending[kind]) return;

    // A level-up pick belongs to ONE cat's screen. After a fight the game shows
    // them one cat after another, and a pick for the second must not land on
    // the first -- so wait until the screen is for the cat it names.
    if (kind == kChoiceLevelUp && g.pending_subject[kind]) {
        const uint64_t here = level_subject(screen);
        if (here && here != g.pending_subject[kind]) return;
    }

    // NODE IDENTITY FIRST, BEFORE THE COUNT AND BEFORE THE NAME.
    //
    // The count and the name describe a screen; this describes WHICH NODE the
    // screen belongs to, and a choice about another node is not a choice about
    // this one however well its shape happens to match. Checking it last would
    // be checking it never: on 2026-08-25 a stale choice passed the count check
    // (both events offered 2 options) and the name check reported the
    // disagreement and obeyed it anyway.
    //
    // Both seeds must be known for this to mean anything. A zero on either side
    // is "not stated" -- a peer that has not entered a node this session, or a
    // pre-19 sender -- and unknown must not read as mismatched.
    //
    // A MISMATCH HOLDS, IT DOES NOT DROP, and the difference is the whole
    // correctness of the common case. The host publishes its choice the instant
    // it clicks, which is routinely BEFORE this peer has entered that node at
    // all -- the client is still finishing the previous one. At that moment the
    // seeds legitimately disagree and the choice is simply not due yet.
    // Dropping here would throw away every choice that arrives early, which is
    // most of them.
    //
    // Deciding a choice is stale needs the one fact this function does not
    // have: that the run has MOVED PAST the node it was made on. That is
    // choice_on_node_skipped's, and it is where the drop lives.
    if (g.pending_seed[kind] && g.here_seed &&
        g.pending_seed[kind] != g.here_seed) {
        if (!g.pending_warned[kind]) {
            g.pending_warned[kind] = true;
            log_line("CHOICE", "holding the host's %s choice %u -- it was made on node"
                               " %016llx and this peer is still in %016llx",
                     kind_name(kind), g.pending_index[kind],
                     (unsigned long long)g.pending_seed[kind],
                     (unsigned long long)g.here_seed);
        }
        return;
    }

    const uintptr_t bo = (kind == kChoiceLevelUp) ? kLvl_OptBegin : kEvt_OptBegin;
    const uintptr_t eo = (kind == kChoiceLevelUp) ? kLvl_OptEnd   : kEvt_OptEnd;

    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(screen, bo, eo, begin, count)) {
        // The screen exists but its options are not built yet. Normal for a
        // frame or two; only worth a line if it never resolves.
        if (!g.pending_warned[kind]) {
            g.pending_warned[kind] = true;
            log_line("CHOICE", "holding the host's %s choice %u -- this peer's option"
                               " list is not built yet",
                     kind_name(kind), g.pending_index[kind]);
        }
        return;
    }

    const uint32_t want  = g.pending_index[kind];
    const uint32_t their = g.pending_count[kind];

    // Count first: if the two lists are different lengths the index means
    // nothing, and saying so beats picking the wrong option.
    if (their && their != count) {
        log_line("CHOICE", "!! host offered %u %s option(s), this peer built %u --"
                           " refusing to choose; the two screens are not the same screen",
                 their, kind_name(kind), count);
        g.pending[kind] = false;
        return;
    }
    if (want >= count) {
        log_line("CHOICE", "!! host chose %s option %u but this peer has only %u --"
                           " refusing", kind_name(kind), want, count);
        g.pending[kind] = false;
        return;
    }

    // Then the name. This is the cross-check that catches "same length,
    // different content", which is the failure an index cannot survive. It is
    // reported and then obeyed: the host DID press that button, and stalling
    // here would be worse than proceeding loudly.
    char mine[48] = {};
    option_name(kind, begin + (uintptr_t)want * kOptStride, mine, sizeof(mine));
    if (g.pending_name[kind][0] && strcmp(mine, g.pending_name[kind]) != 0)
        log_line("CHOICE", "!! %s option %u is '%s' here but '%s' on the host --"
                           " taking it anyway, the lists are the same length",
                 kind_name(kind), want, mine, g.pending_name[kind]);

    if (kind == kChoiceLevelUp) {
        const uint32_t mine_type = option_type(begin + (uintptr_t)want * kOptStride);
        if (g.pending_aux[kind] && mine_type != g.pending_aux[kind])
            log_line("CHOICE", "!! level-up option %u is type %u here but %u on the"
                               " host -- taking it anyway",
                     want, mine_type, g.pending_aux[kind]);
    }

    const bool ok = (kind == kChoiceLevelUp) ? inject_level(want) : inject_event(want);
    g.pending[kind] = false;
    g.pending_warned[kind] = false;
    if (kind == kChoiceLevelUp && g.lvl_queued) {
        const ChoiceMsg next = g.lvl_queue[0];
        memmove(&g.lvl_queue[0], &g.lvl_queue[1], (g.lvl_queued - 1) * sizeof(ChoiceMsg));
        --g.lvl_queued;
        g.pending[kind]         = true;
        g.pending_seed[kind]    = next.node_seed;
        g.pending_index[kind]   = next.index;
        g.pending_count[kind]   = next.count;
        g.pending_aux[kind]     = next.aux;
        g.pending_subject[kind] = next.subject;
        memcpy(g.pending_name[kind], next.name, sizeof(g.pending_name[kind]));
        g.pending_name[kind][sizeof(g.pending_name[kind]) - 1] = '\0';
    }
    if (ok) {
        ++g.applied;
        log_line("CHOICE", "applied the other player's %s choice: option %u/%u ('%s')",
                 kind_name(kind), want, count, mine);
    } else {
        log_line("CHOICE", "!! failed to apply the other player's %s choice %u",
                 kind_name(kind), want);
    }
}

} // namespace

// ---------------------------------------------------------------------------

void choice_set_base(uintptr_t base) {
    g.base = base;
    g.resolved = false;
    g.evt_commit = nullptr;
    g.lvl_click  = nullptr;

    // An armed client that cannot inject is worse than a desync: it swallows
    // clicks it has no way to replace and the screen is permanently dead with
    // nothing to time out. So both call targets must resolve or the whole
    // feature disarms.
    const uintptr_t evt = addr_of(T_EventChoice);
    const uintptr_t lvl = addr_of_call(C_LevelUpClick);
    if (!evt || !lvl) {
        log_line("CHOICE", "!! %s did not resolve by signature"
                           " -- choice replication is OFF",
                 !evt ? "WorldEvent option commit" : "LevelUpScreen button click");
        return;
    }
    g.evt_commit = (fn_cap)evt;
    g.lvl_click  = (fn_cap)lvl;
    g.resolved = true;
}

void choice_init() {
    if (!g.cs_ready) { InitializeCriticalSection(&g.cs); g.cs_ready = true; }
    g.on          = tune::kChoice;
    g.is_client   = (net_role() == NetRole::Client);
    g.world_event = nullptr;
    g.level_screen = nullptr;
    g.pending[0] = g.pending[1] = false;
    g.lvl_queued = 0;
    g.injecting = false;
    g.pq_n = 0;
    g.ctl_we = nullptr;
    g.press_injecting = false;
    if (!g.on) { log_line("CHOICE", "choice replication disabled by net_choice = 0"); return; }
    if (g.is_client && !g.resolved) {
        // Refusing is the safe direction. An armed client SWALLOWS every local
        // click on these screens, so one that cannot inject the host's choice
        // would not desync -- it would sit on a dead screen forever with no
        // way to press anything.
        g.on = false;
        log_line("CHOICE", "!! call targets unresolved -- choice replication is OFF."
                           " This peer keeps its own event and level-up clicks; expect"
                           " the two runs to diverge on the first event.");
        return;
    }
    log_line_lvl(LogLevel::Trace, "CHOICE", "armed -- events and level-ups are answered"
                 " by whoever owns the cat they are about; the other peer follows");
}

void choice_shutdown() {
    if (!g.on) return;
    log_line_lvl(LogLevel::Trace, "CHOICE",
             "done: %u sent, %u applied, %u local click(s) suppressed; presses %u sent,"
             " %u repeated here, %u ignored", g.sent, g.applied, g.swallowed,
             g.presses_sent, g.presses_done, g.presses_eaten);
    g.on = false;
    if (g.cs_ready) { DeleteCriticalSection(&g.cs); g.cs_ready = false; }
}

// --- the world event -------------------------------------------------------

bool choice_on_event_commit(void* cap) {
    if (!g.on || !cap) return true;
    if (g.injecting) return true;           // this is our own injected click

    Guard guard;

    void* we    = nullptr;
    void* entry = nullptr;
    if (!mem_read((const uint8_t*)cap + 8,  &we,    sizeof(we)))    return true;
    if (!mem_read((const uint8_t*)cap + 16, &entry, sizeof(entry))) return true;
    g.world_event = we;

    uint8_t* begin = nullptr; uint32_t count = 0;
    const bool have = read_options(we, kEvt_OptBegin, kEvt_OptEnd, begin, count);

    if (!event_mine(we)) {
        ++g.swallowed;
        log_line("CHOICE", "suppressed a local event click -- the other player answers"
                           " this event (this peer had %u option(s))", have ? count : 0);
        return false;                        // do NOT run the original
    }

    // Ours: name the index, publish it, then let the click through.
    if (!have) {
        log_line("CHOICE", "!! could not read this event's option array -- the peer will"
                           " not be told which option was picked");
        return true;
    }
    const uintptr_t delta = (uintptr_t)((uint8_t*)entry - begin);
    if (delta % kOptStride || delta / kOptStride >= count) {
        log_line("CHOICE", "!! the chosen option is not an element of this event's array"
                           " -- not publishing");
        return true;
    }

    ChoiceMsg m{};
    m.kind  = kChoiceEvent;
    m.index = (uint32_t)(delta / kOptStride);
    m.count = count;
    m.node_seed = follow_here_seed();
    option_name(kChoiceEvent, (const uint8_t*)entry, m.name, sizeof(m.name));
    if (net_send_choice(m)) {
        ++g.sent;
        log_line("CHOICE", "-> event option %u/%u ('%s')", m.index, m.count, m.name);
    }
    return true;
}

void choice_on_event_update(void* world_event) {
    if (!g.on || !world_event) return;
    Guard guard;
    g.world_event = world_event;
    g.event_tick = GetTickCount64();
    // Either role may be waiting: the answer comes from whoever owns the cat.
    apply_pending(kChoiceEvent, world_event);
}

// --- the level-up screen ---------------------------------------------------

bool choice_on_level_select(void* screen, void* opt) {
    if (!g.on || !screen || !opt) return true;
    if (g.injecting) return true;

    Guard guard;
    g.level_screen = screen;

    if (!level_is_mine(screen)) {
        ++g.swallowed;
        log_line("CHOICE", "suppressed a local level-up click -- the other player owns"
                           " this cat and picks its level-up");
        return false;
    }

    // The option arrives as a COPY (the button callback copy-constructs it
    // through sub_14037BBB0), so the index cannot come from pointer arithmetic
    // the way the event's does. Match on content instead: type plus authored
    // name, which is the same identity the wire carries.
    uint8_t* begin = nullptr; uint32_t count = 0;
    if (!read_options(screen, kLvl_OptBegin, kLvl_OptEnd, begin, count)) {
        log_line("CHOICE", "!! could not read the level-up option array -- the peer will"
                           " not be told which option was picked");
        return true;
    }

    char want_name[48] = {};
    option_name(kChoiceLevelUp, (const uint8_t*)opt, want_name, sizeof(want_name));
    const uint32_t want_type = option_type((const uint8_t*)opt);

    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = begin + (uintptr_t)i * kOptStride;
        if (option_type(e) != want_type) continue;
        char nm[48] = {};
        option_name(kChoiceLevelUp, e, nm, sizeof(nm));
        if (strcmp(nm, want_name) == 0) { found = i; break; }
    }
    if (found == UINT32_MAX) {
        log_line("CHOICE", "!! the chosen level-up option (type %u '%s') matches none of"
                           " the %u on offer -- not publishing",
                 want_type, want_name, count);
        return true;
    }

    ChoiceMsg m{};
    m.kind  = kChoiceLevelUp;
    m.index = found;
    m.count = count;
    m.aux   = want_type;
    m.node_seed = follow_here_seed();
    m.subject = level_subject(screen);
    memcpy(m.name, want_name, sizeof(m.name));
    if (net_send_choice(m)) {
        ++g.sent;
        log_line("CHOICE", "-> level-up option %u/%u (type %u '%s') for cat %016llx",
                 m.index, m.count, m.aux, m.name, (unsigned long long)m.subject);
    }
    return true;
}

void choice_on_level_update(void* level_screen) {
    if (!g.on || !level_screen) return;
    Guard guard;
    g.level_screen = level_screen;
    // Either role may be waiting for the other's pick now: the owner of the
    // cat decides, and that can be the client.
    if (!g.pending[kChoiceLevelUp]) return;

    // select_option no-ops once LevelUpScreen+792 is set, so injecting into a
    // screen that already committed would be swallowed by the GAME rather than
    // by us -- silently. Drop the held choice instead, and say so.
    uint8_t committed = 0;
    if (mem_read((const uint8_t*)level_screen + kLvl_Committed, &committed, 1) && committed) {
        if (g.pending[kChoiceLevelUp]) {
            log_line("CHOICE", "!! a level-up choice arrived for a screen that has already"
                               " committed -- dropping it");
            g.pending[kChoiceLevelUp] = false;
        }
        return;
    }
    apply_pending(kChoiceLevelUp, level_screen);
}

// --- receiving -------------------------------------------------------------

void choice_on_message(const ChoiceMsg& m) {
    if (!g.on) return;
    Guard guard;

    // Both kinds come from whoever owns the cat the screen is about, so
    // either role receives either kind now.
    const uint8_t kind = (m.kind == kChoiceLevelUp) ? kChoiceLevelUp : kChoiceEvent;

    // A choice for a node mgmp_follow already gave up on. Refusing it here is
    // the other half of choice_on_node_skipped: that one catches the choice
    // that was already held when the node was discarded, this one catches the
    // choice that arrives afterwards. Holding it would block the next real
    // choice and eventually land on the wrong screen.
    if (was_skipped(m.node_seed)) {
        log_line("CHOICE", "!! a %s choice arrived for node %016llx, which this peer"
                           " passed over without entering -- refusing it. The host"
                           " resolved that node and this peer did not.",
                 kind_name(kind), (unsigned long long)m.node_seed);
        return;
    }

    if (kind == kChoiceLevelUp && g.pending[kind] && m.subject &&
        m.subject != g.pending_subject[kind]) {
        if (g.lvl_queued < State::kLvlQueue) {
            g.lvl_queue[g.lvl_queued++] = m;
            log_line("CHOICE", "<- queued a level-up pick for cat %016llx behind the one "
                               "still waiting", (unsigned long long)m.subject);
            return;
        }
    }
    if (g.pending[kind])
        log_line("CHOICE", "!! a %s choice was still held when another arrived --"
                           " the older one (option %u) is dropped",
                 kind_name(kind), g.pending_index[kind]);

    g.pending[kind]        = true;
    g.pending_seed[kind]   = m.node_seed;
    g.pending_index[kind]  = m.index;
    g.pending_count[kind]  = m.count;
    g.pending_aux[kind]    = m.aux;
    g.pending_warned[kind] = false;
    memcpy(g.pending_name[kind], m.name, sizeof(g.pending_name[kind]));
    g.pending_name[kind][sizeof(g.pending_name[kind]) - 1] = '\0';
    g.pending_subject[kind] = (kind == kChoiceLevelUp) ? m.subject : 0;

    log_line("CHOICE", "<- the other player chose %s option %u/%u ('%s') on node %016llx",
             kind_name(kind), m.index, m.count, g.pending_name[kind],
             (unsigned long long)m.node_seed);

    // Apply immediately if the screen is already up; otherwise the update tick
    // takes it. Doing it here too is what makes the common case land in the
    // same frame the message arrived.
    void* screen = (kind == kChoiceLevelUp) ? g.level_screen : g.world_event;
    if (screen) apply_pending(kind, screen);
}

void choice_on_node_entered(uint64_t node_seed) {
    if (!g.on) return;
    Guard guard;

    // Presses for the node we are LEAVING can never be made now: that screen
    // is gone. Presses for nodes we have not reached yet stay queued.
    const uint64_t leaving = g.here_seed;
    if (leaving && leaving != node_seed) {
        uint32_t w = 0;
        for (uint32_t i = 0; i < g.pq_n; ++i) {
            if (g.pq[i].seed == leaving) {
                log_line("CHOICE", "!! never pressed '%s' on the node just left -- dropped",
                         g.pq[i].name);
                continue;
            }
            g.pq[w++] = g.pq[i];
        }
        g.pq_n = w;
    }
    g.here_seed = node_seed;
    g.ctl_we = nullptr;

    // The cached screens belong to the node just left. Clearing them is not
    // tidiness: choice_on_message applies straight into whichever pointer is
    // held, so a stale one is both a wrong screen and a possible dead object.
    // The update ticks re-cache a live pointer on their very next frame.
    g.world_event  = nullptr;
    g.level_screen = nullptr;

    // A choice already waiting for THIS node becomes due the moment we arrive.
    // Re-arm the "holding" line so the next one is reported afresh rather than
    // suppressed by a warning about the node we just left.
    for (uint8_t kind = 0; kind < 2; ++kind)
        if (g.pending[kind] && g.pending_seed[kind] == node_seed)
            g.pending_warned[kind] = false;
}

void choice_on_node_skipped(uint64_t node_seed) {
    if (!g.on || !node_seed) return;
    Guard guard;
    remember_skipped(node_seed);

    {
        uint32_t w = 0;
        for (uint32_t i = 0; i < g.pq_n; ++i)
            if (g.pq[i].seed != node_seed) g.pq[w++] = g.pq[i];
        g.pq_n = w;
    }
    for (uint8_t kind = 0; kind < 2; ++kind) {
        if (!g.pending[kind] || g.pending_seed[kind] != node_seed) continue;
        log_line("CHOICE", "!! node %016llx was passed over without this peer"
                           " entering it, and a %s choice for it (option %u '%s') was"
                           " still held -- DROPPING it rather than letting it surface"
                           " on a later node. The host resolved that node and this"
                           " peer did not: the two runs have diverged.",
                 (unsigned long long)node_seed, kind_name(kind),
                 g.pending_index[kind], g.pending_name[kind]);
        g.pending[kind]        = false;
        g.pending_warned[kind] = false;
    }
}

// --- following the host out of an event ------------------------------------

bool choice_event_open() {
    if (!g.on || !g.event_tick) return false;
    return GetTickCount64() - g.event_tick < 300;
}

void choice_request_event_exit() {
    if (!g.on) return;
    Guard guard;
    // Not while the other player's answer is still waiting to be applied:
    // pressing continue on an event this peer has not answered would leave it
    // unanswered.
    if (g.pending[kChoiceEvent]) return;
    g.exit_released = g.here_seed;
    press_push(g.here_seed, "Event_ExitButton", "the host has moved on");
}

bool resolve_click() {
    if (g.button_click) return true;
    const uintptr_t click = addr_of_call(C_ButtonClick);
    if (!click) return false;
    g.button_click = (void(*)(void*, bool))click;
    return true;
}

bool choice_on_button_click(void* button) {
    if (!g.on || !button || !net_active()) return true;
    if (g.press_injecting) return true;               // our own repeat of a press

    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name)))
        return true;

    const bool exit   = is_exit(name);
    const bool choice = strcmp(name, "Event_ChoiceButton") == 0;
    const bool chest  = is_chest(name);
    const bool shopx  = strcmp(name, "Shop_ExitButton") == 0;
    if (!exit && !choice && !chest && !shopx) return true;

    Guard guard;
    uint8_t ctl = kHostPeer;
    if (exit || choice) {
        if (!g.world_event || !choice_event_open()) return true;
        ctl = event_controller(g.world_event);
    } else if (shopx && !chest_node(follow_here_type())) {
        return true;                                  // an ordinary shop: both shop
    }

    if (exit && g.exit_released && g.exit_released == g.here_seed) return true;
    if (ctl != self_peer()) {
        ++g.presses_eaten;
        if (g.presses_eaten == 1 || (g.presses_eaten % 10) == 0)
            log_line("CHOICE", "ignored '%s' on this peer -- the other player controls"
                               " this screen (%u so far)", name, g.presses_eaten);
        return false;
    }
    if (choice) return true;              // its CHOICE is published by the commit

    PressMsg m{};
    m.node_seed = g.here_seed;
    strncpy_s(m.name, name, _TRUNCATE);
    if (net_send_press(m)) {
        ++g.presses_sent;
        log_line("CHOICE", "-> pressed '%s'", name);
    }
    return true;
}

void choice_on_press(uint8_t from, const PressMsg& m) {
    if (!g.on) return;
    char name[48];
    memcpy(name, m.name, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    if (!mirrorable(name)) {
        log_line("CHOICE", "!! peer %u asked this peer to press '%s', which is not a"
                           " button that is ever mirrored -- refused", (unsigned)from, name);
        return;
    }
    Guard guard;
    if (was_skipped(m.node_seed)) {
        log_line("CHOICE", "!! a press of '%s' arrived for a node this peer passed over"
                           " -- refused", name);
        return;
    }
    if (is_exit(name)) g.exit_released = m.node_seed;
    press_push(m.node_seed, name, "the other player pressed it");
}

void choice_on_button_update(void* button) {
    if (!g.on || !button || !g.pq_n) return;
    Guard guard;
    State::Press& f = g.pq[0];
    // Due only on the node it was pressed on. A press for a node this peer has
    // not reached yet waits; one for a node it left was dropped on the way out.
    if (f.seed && g.here_seed && f.seed != g.here_seed) return;

    char name[64];
    if (!mem_read_std_string((const uint8_t*)button + kBtn_Name, name, sizeof(name)))
        return;
    if (strcmp(name, f.name) != 0) return;

    const ULONGLONG now = GetTickCount64();
    if (!f.due) f.due = now;
    if (is_exit(f.name) && g.pending[kChoiceEvent]) return;   // the answer first
    if (now - g.press_last < 450) return;    // one press, then let the screen react
    if (!resolve_click()) {
        log_line("CHOICE", "!! Button::Click did not resolve -- cannot press '%s'", f.name);
        press_pop();
        return;
    }

    ++f.tries;
    g.press_last = now;
    log_line("CHOICE", "pressing '%s' for the other player", f.name);
    g.press_injecting = true;
    g.button_click(button, false);
    g.press_injecting = false;

    // A chest item is one press. Continue is retried until the event is
    // actually gone (choice_pump), because Button::Click has guards of its own
    // and a press during the result's animation can be refused.
    if (!is_exit(f.name)) { ++g.presses_done; press_pop(); }
}

void choice_pump() {
    if (!g.on || !g.pq_n) return;
    Guard guard;
    State::Press& f = g.pq[0];
    const ULONGLONG now = GetTickCount64();
    if (is_exit(f.name) && f.tries && !choice_event_open()) {
        ++g.presses_done;
        log_line_lvl(LogLevel::Trace, "CHOICE", "the event closed after %u press(es)",
                     f.tries);
        press_pop();
        return;
    }
    if (f.tries >= 12) {
        log_line("CHOICE", "!! pressed '%s' %u times and the screen did not react --"
                           " giving up on it", f.name, f.tries);
        press_pop();
        return;
    }
    // Due here, and the button never showed up. Long enough for any screen
    // transition; short enough that one stale press cannot hold the queue for
    // the rest of the run.
    if (f.seed == g.here_seed && f.due == 0) f.due = now;
    if (f.due && f.tries == 0 && now - f.due > 30000) {
        log_line("CHOICE", "!! '%s' never appeared on this peer in 30 s -- dropped",
                 f.name);
        press_pop();
    }
}

bool choice_event_status(char* cat, uint32_t cap, bool* mine) {
    if (cat && cap) cat[0] = '\0';
    if (mine) *mine = false;
    if (!g.on || !net_active() || !choice_event_open() || !g.world_event) return false;
    Guard guard;
    const uint8_t who = event_controller(g.world_event);
    if (mine) *mine = (who == self_peer());
    if (cat && cap && g.ctl_cat) {
        static PartyRow rows[64];
        const uint32_t n = party_rows(rows, 64);
        for (uint32_t i = 0; i < n; ++i)
            if (rows[i].id == g.ctl_cat) { strncpy_s(cat, cap, rows[i].name, _TRUNCATE); break; }
    }
    return true;
}

} // namespace mgmp
