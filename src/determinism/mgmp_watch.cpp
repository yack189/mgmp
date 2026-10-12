// mgmp_watch.cpp -- see mgmp_watch.h.

#include "mgmp_watch.h"
#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

constexpr uint32_t kSites = 64;
// Call-chain signatures: the first kDepth game-module return addresses above
// the writing instruction. Counted alongside the per-site totals so a site
// reached from two places (a decision and a per-frame display) shows as two.
constexpr uint32_t kChains = 96;
constexpr uint32_t kDepth  = 6;

struct State {
    uint64_t* addr = nullptr;
    uintptr_t base = 0;
    uintptr_t self_lo = 0, self_hi = 0;   // mgmp.dll, for "ours" vs "game"
    PVOID     veh = nullptr;
    DWORD     game_tid = 0;

    // Written only by the handler, which runs on the game thread -- the only
    // thread whose debug registers are armed -- and read by watch_report on
    // the same thread. No lock needed.
    uintptr_t site[kSites] = {};
    uint32_t  hits[kSites] = {};
    uint32_t  other = 0;
    uint32_t  total = 0;

    uintptr_t chain[kChains][kDepth] = {};
    uint32_t  chain_hits[kChains] = {};
    uint32_t  chains = 0;
};

State g;

LONG CALLBACK on_step(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;   // not our watchpoint

    const uintptr_t rip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    ++g.total;
    uint32_t i = 0;
    for (; i < kSites; ++i) {
        if (g.site[i] == rip) { ++g.hits[i]; break; }
        if (!g.site[i]) { g.site[i] = rip; g.hits[i] = 1; break; }
    }
    if (i == kSites) ++g.other;

    // Who called it. Unwinding from inside a vectored handler walks through
    // KiUserExceptionDispatcher into the faulting thread's own frames -- the
    // same thing mgmp_crash relies on. Keep the first game-module frames.
    void* fr[40] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 40, fr, nullptr);
    uintptr_t key[kDepth] = {};
    uint32_t  k = 0;
    for (USHORT j = 0; j < n && k < kDepth; ++j) {
        const uintptr_t a = (uintptr_t)fr[j];
        if (a >= g.base && a < g.base + 0x2000000) key[k++] = a - g.base;
    }
    uint32_t ci = 0;
    for (; ci < g.chains; ++ci)
        if (memcmp(g.chain[ci], key, sizeof(key)) == 0) { ++g.chain_hits[ci]; break; }
    if (ci == g.chains && g.chains < kChains) {
        memcpy(g.chain[g.chains], key, sizeof(key));
        g.chain_hits[g.chains++] = 1;
    }

    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct ArmArgs { HANDLE thread; uint64_t* addr; bool ok; };

DWORD WINAPI arm_thread(void* p) {
    ArmArgs* a = (ArmArgs*)p;
    if (SuspendThread(a->thread) == (DWORD)-1) return 0;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(a->thread, &c)) {
        c.Dr0 = (DWORD64)a->addr;
        c.Dr6 = 0;
        // L0 = bit 0; RW0 (bits 16-17) = 01 write; LEN0 (bits 18-19) = 10 8 bytes.
        c.Dr7 = (c.Dr7 & ~0x000F0003ull) | 0x1ull | (0x1ull << 16) | (0x2ull << 18);
        a->ok = SetThreadContext(a->thread, &c) != 0;
    }
    ResumeThread(a->thread);
    return 0;
}

} // namespace

void watch_arm(uint64_t* addr, uintptr_t game_base) {
    if (!addr) return;
    if (g.addr == addr && g.game_tid == GetCurrentThreadId()) return;
    g.base = game_base;

    if (!g.self_lo) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&watch_arm, &self);
        g.self_lo = (uintptr_t)self;
        g.self_hi = g.self_lo + 0x2000000;
    }
    if (!g.veh) g.veh = AddVectoredExceptionHandler(1, on_step);

    HANDLE me = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                           FALSE, GetCurrentThreadId());
    if (!me) { log_line("WATCH", "!! could not open the game thread -- no watchpoint"); return; }
    ArmArgs a{ me, addr, false };
    HANDLE t = CreateThread(nullptr, 0, arm_thread, &a, 0, nullptr);
    if (t) { WaitForSingleObject(t, 2000); CloseHandle(t); }
    CloseHandle(me);

    if (!a.ok) { log_line("WATCH", "!! setting the debug register failed -- no watchpoint"); return; }
    g.addr = addr;
    g.game_tid = GetCurrentThreadId();
    memset(g.site, 0, sizeof(g.site));
    memset(g.hits, 0, sizeof(g.hits));
    g.other = g.total = 0;
    log_line("WATCH", "write-watchpoint on the simulation stream (%p) -- every write is "
                      "counted by site and printed at each turn boundary", (void*)addr);
}

void watch_report(uint32_t turn) {
    if (!g.addr || GetCurrentThreadId() != g.game_tid) return;
    char line[900];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "turn %u: %u write(s):", turn, g.total);
    for (uint32_t i = 0; i < kSites && g.site[i] && n > 0 && n < (int)sizeof(line) - 40; ++i) {
        const uintptr_t r = g.site[i];
        const bool game = r >= g.base && r < g.base + 0x2000000;
        const bool ours = r >= g.self_lo && r < g.self_hi;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %s+%06llX x%u",
                         game ? "" : ours ? "mgmp" : "?",
                         (unsigned long long)(game ? r - g.base : ours ? r - g.self_lo : r),
                         g.hits[i]);
    }
    if (g.other) _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " (+%u more)", g.other);
    log_line("WATCH", "%s", line);
    for (uint32_t ci = 0; ci < g.chains; ++ci) {
        char cl[256];
        int m = _snprintf_s(cl, sizeof(cl), _TRUNCATE, "  x%-5u", g.chain_hits[ci]);
        for (uint32_t d = 0; d < kDepth && g.chain[ci][d] && m > 0; ++d)
            m += _snprintf_s(cl + m, sizeof(cl) - m, _TRUNCATE, " %06llX",
                             (unsigned long long)g.chain[ci][d]);
        log_line("WATCH", "%s", cl);
    }
    memset(g.chain, 0, sizeof(g.chain));
    memset(g.chain_hits, 0, sizeof(g.chain_hits));
    g.chains = 0;
    memset(g.site, 0, sizeof(g.site));
    memset(g.hits, 0, sizeof(g.hits));
    g.other = g.total = 0;
}

} // namespace mgmp
