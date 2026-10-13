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
// DEPARTING. The sign that departs (four cats in the box) is a way out like
// the NPC map: either player presses it, both go into the class chooser, and
// every press in there goes through the host and comes back to both -- either
// player may pick the collars. The chooser's start button starts the HOST's
// adventure only; a client waits in its chooser until the host's run is
// published and then loads it (mgmp_house's reload, the same as a new day).
//
// THE ADVENTURE'S SHOPS are the host's. A client's presses there are refused;
// the host's purchases are pressed on the client as well, with its stream, so a
// candy bought by the host opens the same level-up screen on both machines and
// the cat's owner picks it (mgmp_choice).
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
bool town_awaiting_run();  // client: the host started the adventure, loading it
bool town_presses_pending(); // client: host presses still waiting to be made here
bool town_shop_refused_recently();   // client: pressed in the host's shop
void town_shutdown();

} // namespace mgmp
