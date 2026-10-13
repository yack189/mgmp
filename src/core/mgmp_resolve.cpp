// mgmp_resolve.cpp -- see mgmp_resolve.h.
#include "mgmp_resolve.h"

#include "mgmp_log.h"
#include "mgmp_sigscan.h"
#include "mgmp_sigs.generated.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

namespace mgmp {
namespace {

// The generated tables are indexed by the enums, so a length mismatch means
// somebody added a target and did not re-run gen_sigs.py. Catch it at compile
// time rather than by silently resolving the wrong entry.
static_assert(sizeof(kTargetSigs) / sizeof(kTargetSigs[0]) == (size_t)T_COUNT,
              "mgmp_sigs.generated.h is stale -- re-run mod/tools/ida/gen_sigs.py");
static_assert(sizeof(kCallSigs) / sizeof(kCallSigs[0]) == (size_t)C_COUNT,
              "mgmp_sigs.generated.h is stale -- re-run mod/tools/ida/gen_sigs.py");
static_assert(sizeof(kSigData) / sizeof(kSigData[0]) == (size_t)D_COUNT,
              "mgmp_sigs.generated.h is stale -- re-run mod/tools/ida/gen_sigs.py");

// Criticality. The question is NOT "how much do we like this feature" but
// "if this address goes missing, does the mod fail VISIBLY or SILENTLY".
//
// A missing draw call turns a cursor off -- visible, and the module already
// says so. A missing GetChoice means remote decisions are never injected and
// both peers quietly play their own game: that is a silent wrong game, which
// CLAUDE.md ranks as strictly worse than a stall. Those four are the seam
// lockstep is built on, so they refuse.
bool is_critical(Target t) {
    switch (t) {
    case T_NextTurn:     // the turn barrier and the hash exchange
    case T_GetChoice:    // the injection seam for remote decisions
    case T_ApplyAction:  // the command boundary
    case T_FrameBegin:   // the socket pump
        return true;
    default:
        return false;
    }
}

// --- the SDL_DYNAPI slots ---------------------------------------------------
//
// Each from one game call site, read off the pinned build (4127cd6a):
//   SDL_GL_SwapWindow          the present, 0x1409B8018: `mov rcx,[rsi+0DD0h];
//                              test rcx,rcx; jz; call SDL_GL_SwapWindow; call ..`
//   SDL_GetWindowSize          0x140A5C463, with the flags query after it
//   SDL_GetWindowSizeInPixels  0x1409B71DD, ApplicationBase's size refresh
// Each pattern is unique in .text with the call rel32s wildcarded and the
// struct displacements kept, as gen_sigs.py would.
struct SdlSlotDesc {
    const char* name;
    uint32_t    site_hint;   // rva of the pattern's start on the pinned build
    const char* pattern;
    uint16_t    call_off;    // offset of the E8 within the pattern
    uint32_t    slot_hint;   // kRva_* -- the cross-check, logged on drift
};
const SdlSlotDesc kSdlSlots[S_COUNT] = {
    { "SDL_GL_SwapWindow slot", 0x009B8018,
      "48 8B 8E D0 0D 00 00 48 85 C9 74 05 E8 ? ? ? ? E8", 12, kRva_SdlSwapSlot },
    { "SDL_GetWindowSize slot", 0x00A5C463,
      "48 8B 0F 4C 8D 84 24 90 00 00 00 48 8D 94 24 98 00 00 00 E8 ? ? ? ? 48 8B 0F E8 ? ? ? ? A8 40",
      19, kRva_SdlGetWindowSizeSlot },
    { "SDL_GetWindowSizeInPixels slot", 0x009B71DD,
      "4C 8D 44 24 34 48 8B 89 D0 0D 00 00 48 8D 54 24 30 33 FF 48 89 7C 24 30 E8",
      24, kRva_SdlGetWindowSizePxSlot },
};
uintptr_t g_sdl[S_COUNT];

// Reads a rel32 and the bytes around it through mem_read-free plain loads: the
// image is mapped and these are inside .text, already bounds-checked.
uintptr_t sdl_slot_from(uintptr_t module_base, const SigRegion& text, const SdlSlotDesc& d,
                        char* why, size_t why_size) {
    SigTargetDesc t{ d.site_hint, d.name, d.pattern };
    const SigResolution r = sig_resolve(module_base, text, t);
    if (!r.ok) { _snprintf_s(why, why_size, _TRUNCATE, "%s", r.why); return 0; }
    const uintptr_t lo = (uintptr_t)text.base, hi = lo + text.size;
    const uintptr_t call = r.addr + d.call_off;
    if (call + 5 > hi || *(const uint8_t*)call != 0xE8) {
        _snprintf_s(why, why_size, _TRUNCATE, "the call site is not a call");
        return 0;
    }
    int32_t rel = 0;
    memcpy(&rel, (const void*)(call + 1), 4);
    const uintptr_t thunk = call + 5 + (intptr_t)rel;
    const uint8_t* tb = (const uint8_t*)thunk;
    if (thunk < lo || thunk + 7 > hi || tb[0] != 0x48 || tb[1] != 0xFF || tb[2] != 0x25) {
        _snprintf_s(why, why_size, _TRUNCATE, "the call does not land on a `jmp [rip+x]` thunk");
        return 0;
    }
    int32_t disp = 0;
    memcpy(&disp, tb + 3, 4);
    const uintptr_t slot = thunk + 7 + (intptr_t)disp;
    // The slot must be inside the image, and outside .text.
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module_base;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(module_base + dos->e_lfanew);
    const uintptr_t img_hi = module_base + nt->OptionalHeader.SizeOfImage;
    if (slot < module_base || slot + 8 > img_hi || (slot >= lo && slot < hi)) {
        _snprintf_s(why, why_size, _TRUNCATE, "the thunk's slot is outside the image's data");
        return 0;
    }
    if (slot - module_base != d.slot_hint)
        _snprintf_s(why, why_size, _TRUNCATE, "MOVED %08X -> %08X",
                    d.slot_hint, (unsigned)(slot - module_base));
    else
        why[0] = 0;
    return slot;
}

uintptr_t g_target[T_COUNT];
uintptr_t g_call  [C_COUNT];
uintptr_t g_data  [D_COUNT];

int  g_resolved = 0, g_moved = 0, g_failed = 0;
bool g_pinned   = true;
bool g_ready    = false;

// One target, one log line -- but only when there is something to say. On the
// pinned build this is silent for all 49, which is the point.
void note(const char* kind, const char* name, const SigResolution& r) {
    if (!r.ok) {
        log_raw("  [!] %s %-34s UNRESOLVED: %s", kind, name, r.why);
        return;
    }
    if (r.drift != 0)
        log_raw("  [~] %s %-34s %s", kind, name, r.why);
}

} // namespace

bool resolve_init(uintptr_t module_base) {
    memset(g_target, 0, sizeof(g_target));
    memset(g_call,   0, sizeof(g_call));
    memset(g_data,   0, sizeof(g_data));
    memset(g_sdl,    0, sizeof(g_sdl));
    g_resolved = g_moved = g_failed = 0;
    g_pinned   = true;
    g_ready    = false;

    SigRegion text;
    if (!sigscan_text_section(module_base, &text)) {
        log_raw("[!] could not find an executable section -- cannot resolve anything");
        return false;
    }
    sigscan_profile(text);

    log_raw("resolve: .text rva %08X, %.1f MB, scanner = %s",
            text.rva, text.size / (1024.0 * 1024.0), sigscan_cpu_name());

    bool critical_lost = false;

    for (int i = 0; i < T_COUNT; ++i) {
        // Anchors are chosen against the profile, which sig_parse could not see
        // when the table was a literal; sig_resolve re-parses, so this is free.
        const SigResolution r = sig_resolve(module_base, text, kTargetSigs[i]);
        note("target", kTargetSigs[i].name, r);
        if (r.ok) {
            g_target[i] = r.addr;
            ++g_resolved;
            if (r.drift) { ++g_moved; g_pinned = false; }
        } else {
            ++g_failed;
            if (is_critical((Target)i)) {
                log_raw("[!] %s is CRITICAL -- lockstep cannot be correct without it",
                        kTargetSigs[i].name);
                critical_lost = true;
            }
        }
    }

    for (int i = 0; i < C_COUNT; ++i) {
        const SigResolution r = sig_resolve(module_base, text, kCallSigs[i]);
        note("call  ", kCallSigs[i].name, r);
        if (r.ok) {
            g_call[i] = r.addr;
            ++g_resolved;
            if (r.drift) { ++g_moved; g_pinned = false; }
        } else {
            ++g_failed;
        }
    }

    for (int i = 0; i < D_COUNT; ++i) {
        const SigResolution r = sig_resolve_data(module_base, text, kSigData[i]);
        note("data  ", kSigData[i].name, r);
        if (r.ok) {
            g_data[i] = r.addr;
            ++g_resolved;
            if (r.drift) { ++g_moved; g_pinned = false; }
        } else {
            ++g_failed;
        }
    }

    for (int i = 0; i < S_COUNT; ++i) {
        char why[128] = {0};
        g_sdl[i] = sdl_slot_from(module_base, text, kSdlSlots[i], why, sizeof(why));
        if (!g_sdl[i]) {
            log_raw("  [!] data   %-34s UNRESOLVED: %s", kSdlSlots[i].name, why);
            ++g_failed;
        } else {
            ++g_resolved;
            if (why[0]) {
                log_raw("  [~] data   %-34s %s", kSdlSlots[i].name, why);
                ++g_moved; g_pinned = false;
            }
        }
    }

    g_ready = true;

    if (g_pinned && !g_failed) {
        log_raw("resolve: all %d addresses matched their pinned RVAs "
                "-- this is the build the signatures were generated from.",
                g_resolved);
    } else {
        log_raw("resolve: %d resolved, %d MOVED, %d failed.",
                g_resolved, g_moved, g_failed);
        if (g_moved)
            log_raw("[~] the game has been updated and the signatures followed it. "
                    "Re-run mod/tools/ida/gen_sigs.py against the new .i64 to "
                    "re-pin the hints and shrink startup back to zero scanning.");
    }

    return !critical_lost;
}

uintptr_t addr_of(Target t) {
    if (!g_ready || t < 0 || t >= T_COUNT) return 0;
    return g_target[t];
}

uintptr_t addr_of_call(Call c) {
    if (!g_ready || c < 0 || c >= C_COUNT) return 0;
    return g_call[c];
}

uintptr_t addr_of_data(DataSym d) {
    if (!g_ready || d < 0 || d >= D_COUNT) return 0;
    return g_data[d];
}

uintptr_t addr_of_sdl_slot(SdlSlot s) {
    if (!g_ready || s < 0 || s >= S_COUNT) return 0;
    return g_sdl[s];
}

bool resolve_is_pinned_build() { return g_pinned; }

void resolve_counts(int* resolved, int* moved, int* failed) {
    if (resolved) *resolved = g_resolved;
    if (moved)    *moved    = g_moved;
    if (failed)   *failed   = g_failed;
}

} // namespace mgmp
