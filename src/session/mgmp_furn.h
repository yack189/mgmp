#pragma once
// mgmp_furn -- the house's furniture, the same on both machines.
//
// A piece of furniture IS its HouseInventory record: item name (+0x08), the
// grid it stands on by name (+0x30, empty = in storage), its cell (+0x50), its
// mirror (+0x58) and draw order (+0x60). The build menu is not state of its
// own: FurnitureBuildingUI's update rebuilds it every frame from "every record
// with no piece" (sub_1401A5AB0), so storage needs no help.
//
// Each player places, moves and stores freely, with their own build drawer --
// nothing drags the other player anywhere. Every 200 ms this peer compares its
// records with the last look; a record whose location changed, and whose piece
// is not in someone's hand, goes to the other peer as FURN {name, from, to}.
// The other peer finds the record with that name at that old location, puts
// its piece away (unplace + destroy, what the game does when a piece is dropped
// back on the menu), rewrites the record, and -- if it now stands somewhere --
// creates the piece again from the record, which places it exactly the way the
// house places its furniture when it loads.
//
// Record ids are minted per process, so a record is found by name + location,
// never by id. Poop is a record too and is mgmp_house's.
//
// ONE HAND AT A TIME. A piece picked up off a grid is told to the other player
// at once (op Hold, with where it stood); there it turns see-through and the
// pick-up (sub_1402ED690, T_FurnPickup) is refused until the Release that
// follows the move. Two players who grab the same piece in the same instant
// both put it down; the host then finds the client's move starting from where
// the host had just moved it from, and sends the client a move from the
// client's spot to the host's -- the host's placement wins.
//
// THE WALLET. The house's coins and food (HouseInventory+0xB4/+0xB0) are the
// host's: it sends them on change and every 2 s, and the client writes them
// over its own whenever no press of the host's is still waiting to be made
// here. Shops opened from the house are pressed by the host (mgmp_town).
#include <cstdint>

namespace mgmp {

struct FurnMsg;

void furn_set_base(uintptr_t base);
// From the House::update hook, after the original: watch, and apply what the
// other player did.
void furn_on_house_update(void* house);
void furn_on_message(uint8_t from, const FurnMsg& m);
// T_FurnPickup: false = the other player has this piece in hand; refuse it.
bool furn_pickup_allowed(void* piece);
void furn_shutdown();

} // namespace mgmp
