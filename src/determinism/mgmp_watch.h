// mgmp_watch.h -- a hardware write-watchpoint on the simulation stream.
//
// WHY. The RNG hooks (mgmp_rng) see every draw that goes through randint /
// randfloat / rand2 / RollChance. They cannot see an INLINED xoshiro step --
// the game writes TLS+0x178 directly in a few places (NextTurn's Fisher-Yates
// is one). A draw like that on one peer only shows up in a recording as the
// next hooked draw's pre-state differing, with nothing in between to blame.
// Measured 2026-10-12: Move then Spit, identical through turn 0, the first
// draw of turn 1 already from different states on the two peers, and the local
// aim preview ruled out by hooking it.
//
// HOW. Debug register 0 on the game thread, set to break on any 8-byte WRITE to
// TLS+0x178 (the stream's first word -- every step rewrites all four, so the
// first is enough). A vectored handler counts the faulting instruction (the
// one AFTER the write, which is what the CPU reports) per module offset, and
// the turn boundary prints the histogram. Diffing the two peers' lines names
// the site that runs on one and not the other.
//
// Only armed with debug.record on: every sim-stream write becomes an exception,
// a few hundred per turn, which is fine for a diagnostic and not for play.
#pragma once

#include <cstdint>

namespace mgmp {

// From the game thread, at battle start. Idempotent per address.
void watch_arm(uint64_t* stream_first_word, uintptr_t game_base);

// From the game thread at a turn boundary: log this turn's sites, then clear.
void watch_report(uint32_t turn);

} // namespace mgmp
