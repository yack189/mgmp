#pragma once
// mgmp_town -- the NPCs: going to see one, what is said there, and their shops.
//
// THE WAY OUT OF THE HOUSE. The house's "map" drawer (a HouseDrawerPanel whose
// own name is "map") opens the NPC map, an NPCMapDrawer whose buttons each go
// to one NPC's office (the NPCOffice scene, on top of the House scene). Either
// player may go; both go. A button on that path is named by what it RUNS --
// its std::function's class, read from the callback vtable's own RTTI -- plus
// its name, a tag and an ordinal, so the same button is found on the other
// machine without trusting a pointer.
//
// INSIDE AN OFFICE, the host presses. A client's press there is not run; it is
// sent to the host as a request, the host presses that button, and that press
// comes back like any other. One player at a time is what keeps a shop's coins
// and stock the same on both. Each press carries the host's simulation stream
// as it stood just before it, so whatever the press rolls -- a shop's stock,
// an NPC's line -- rolls the same here.
//
// DIALOGS. NPCDialogController (and Tutorial, which shares its update) reads
// every click from one input action. On the client the scene's input is
// blocked for the length of that update, and the host's clicks arrive instead:
// a line closed, a reveal skipped, a wait_for "click" answered -- applied in
// order, each when this peer's dialog is showing what it applies to.
#include <cstdint>

namespace mgmp {

struct TownPressMsg;
struct DialogMsg;

void town_set_base(uintptr_t base);

// Button::update, before and after the original. Before: the ordinal of this
// button among this frame's buttons of the same kind. After: press a queued
// press if this is its button.
void town_on_button_pre(void* button);
void town_on_button_post(void* button);
// Button::Click. False = swallow (a client's press inside an office, sent to
// the host instead).
bool town_on_button_click(void* button);
void town_on_press(uint8_t from, const TownPressMsg& m);

// NPCDialogController::update and its speech bubble, around the originals.
void town_dlg_update_pre(void* ctrl);
void town_dlg_update_post(void* ctrl);
void town_dlg_bubble_pre(void* ctrl, void* cmd);
void town_dlg_bubble_post(void* ctrl, void* cmd);
void town_on_dialog(uint8_t from, const DialogMsg& m);

void town_pump();          // once a frame
bool town_in_office();     // for the on-screen notice
void town_shutdown();

} // namespace mgmp
