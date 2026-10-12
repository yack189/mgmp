// mgmp_party.h -- who drives which cat.
//
// The original control split was "human cats in roster order, host first",
// which a player cannot influence. This replaces it with an explicit table,
// CatData id -> peer id, that the PLAYERS edit (the lobby's party panel; later
// the house's party box) and that the battle split and the level-up screens
// both read.
//
// HOST-AUTHORITATIVE, and the reason is the battle. Both peers derive the
// control split independently at the first turn boundary (mgmp_lockstep), so
// both must hold byte-identical tables at that moment. The host owns the only
// real table and broadcasts it (OWNERS) whenever it changes and at every node
// entry, ahead of ENTERNODE on the same TCP stream; a client asks for a change
// (CLAIM) and never edits its own copy, so the copy it battles with is always
// one the host sent. mgmp_lockstep's verify_control still cross-checks the
// result and halts on any cat claimed twice or by nobody.
//
// LINKING A BATTLE CHARACTER TO ITS CAT. The table is keyed by CatData id, and
// the battle's roster holds Character objects. The link is discovered at run
// time rather than assumed: each human Character is scanned for a pointer to
// one of the run's CatData objects (resolved through the game's own registry),
// and the offset is accepted only if it links EVERY human cat to a distinct
// run cat. If that fails, roster order is matched to run-list order, and if
// even the counts disagree the old automatic split is used and the log says so.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mgmp {

struct OwnersMsg;
struct ClaimMsg;

void party_set_base(uintptr_t base);
void party_init();       // session ready
void party_shutdown();

// Once a frame, from the session tick (the game thread). Refreshes the cached
// run list the panel shows and, on the host, assigns defaults to new cats and
// publishes the table when it changed.
void party_pump();

// HOST: publish now, whether or not it changed -- called right before the host
// announces a node, so the table rides ahead of ENTERNODE.
void party_publish(const char* why);

void party_on_owners(const OwnersMsg& m);               // client
void party_on_claim(uint8_t from, const ClaimMsg& m);   // host

// The panel's click. Host: applies and publishes. Client: sends CLAIM.
void party_request(uint64_t cat_id, uint8_t owner_peer);

// Owner of a cat by CatData id; kNoPeer if the table does not name it.
uint8_t party_owner(uint64_t cat_id);

// Does THIS peer drive the cat whose CatData is `cat`? False when the table
// does not know it.
bool party_owns_catdata(const void* cat);
uint64_t party_catdata_id(const void* cat);   // 0 if unreadable

// For mgmp_lockstep's snapshot. `chars` are the human Characters in roster
// order. Fills local_out[k] (k < humans) and returns true when the table could
// decide every one; false means "use the automatic split".
bool party_split(const void* const* chars, uint32_t humans, bool* local_out);

// --- the panel's view -------------------------------------------------------
struct PartyRow {
    uint64_t id = 0;
    char     name[48] = {};
    uint8_t  owner = 0xFF;
};
// Copies the cached run list (at most `cap` rows). 0 outside an adventure.
uint32_t party_rows(PartyRow* out, uint32_t cap);

// Whether the table may be edited right now: in a run, on the map, not in a
// battle. The panel greys itself otherwise.
bool party_editable();

} // namespace mgmp
