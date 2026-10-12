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

} // namespace mgmp
