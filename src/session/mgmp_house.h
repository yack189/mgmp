#pragma once
// mgmp_house -- the shared house.
//
// The adventure is synced by determinism (battles) and by replicating choices
// (map, events, level-ups). The house has neither property: it re-seeds
// nothing (see CLAUDE.md, "The house -- serializable, but NOT
// choice-replicable"), its cats wander on the PRESENTATION stream at the
// frame rate, and two players act in it at once. So it is built up in layers,
// each of which stands on its own:
//
//   1. WHERE THE OTHER PLAYER IS POINTING -- in house WORLD coordinates. Each
//      player moves their own camera between rooms, so a fraction of the
//      window would point at different furniture on the two machines.
//      House::update's first three calls are exactly the conversion the game
//      itself uses for clicks:
//
//          cam   = sub_140978330(*(House+0x20))   // the scene's active camera
//          world = sub_1409796D0(cam, &out)       // inverse(VP) * mouse NDC
//
//      The receiver runs the forward half -- sub_1409785E0(cam) is VP,
//      column-major -- and turns the NDC back into a fraction of its content
//      rect through the same four NdcRange floats sub_140986EE0 used.
//
// Later layers (the cats, grabbing and throwing, leaving the house, the day
// boundary) build on the same tick.
#include <cstdint>

namespace mgmp {

struct HouseCatsMsg;
struct HouseGrabMsg;

void house_set_base(uintptr_t base);

// From the House::update hook, AFTER the original, every frame the house runs.
void house_on_update(void* house);

// The house ticked in the last quarter second: this player is in the house.
bool house_active();

// This player's mouse in house world coordinates, as of the last tick.
bool house_mouse_world(double& x, double& y);

// A house world point as a fraction of THIS player's content rect (0..1,
// origin top-left -- the convention mgmp_overlay draws in). False when this
// player is not in the house or the camera could not be read.
bool house_world_to_frac(double x, double y, float& nx, float& ny);

// --- 2. THE CATS ---------------------------------------------------------------
//
// One authority per cat. The host runs every cat by default; a player who
// picks one up runs that one until it has landed and come to rest, then hands
// it back (MSG_HOUSEGRAB). Whoever is authoritative sends what its cats are
// doing twenty times a second (MSG_HOUSECATS): entity position, mirror, draw
// order, the animator's (pose, name) and facing, and which placement area --
// box, bed -- it is in.
//
// The other peer does NOT run those cats. Their HouseCat::update and
// HouseCatPhysics::update are skipped, and the animator setter refuses the
// cat's own changes, so the local AI cannot walk one off, sit it somewhere
// else, or make it poop; the received state is written straight into the
// entity and the animator instead. A cat nobody has described for three
// seconds runs locally again, so a dropped peer never leaves a frozen house.
//
// Selecting a cat to read its stats is the click manager's business and stays
// on each machine: nothing here touches it.
void house_on_cats(uint8_t from, const HouseCatsMsg& m);
void house_on_grab(uint8_t from, const HouseGrabMsg& m);

// The hooks. Each returns true when the original should run.
bool house_cat_update(void* house_cat);
bool house_cat_physics(void* physics);
bool house_cat_late(void* house_cat);   // HouseCat late_update (slot 11): the
                                        // cat-on-cat "swat" AI; skipped likewise
bool house_anim_allowed(void* animator, void* consumed_string);

// The Button::Click hook asks this first. For now the sign that ends the day
// (or departs) is the host's alone: each house would otherwise roll its own
// night. False = swallow the press.
bool house_on_button_click(void* button);
bool house_sign_refused_recently();   // for the on-screen notice

void house_shutdown();

} // namespace mgmp
