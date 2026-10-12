// mgmp_lobby.h -- the player-facing co-op UI: the MULTIJUGADOR button on the
// title screen, the party window behind it, and the small in-game indicators
// (who you are playing with, whose turn it is).
//
// WHY THIS IS NOT THE DEBUG PANEL
//
// mgmp_ui's panel is an engineer's view -- English, every counter the session
// keeps, and hidden behind F1. This is what a player who has never opened the
// panel sees, so it is Spanish, it shows four things, and it decides on its own
// when to be on screen. It shares the panel's ImGui context and draw site
// because there is exactly one of each (see mgmp_ui.h), and is called from
// inside the panel's frame.
//
// WHAT IT DRIVES
//
// Nothing new. "Crear partida" / "Unirse" / "Salir" are the panel's own
// session_request_host / _join / _disconnect, which already install the co-op
// hooks late for a process launched with role = off. "Comenzar" is the main
// menu's Play, pressed through the same Button::Click the client's auto-Play
// uses (savefile_request_local_play). Pressing the game's own Play instead of
// "Multijugador" never opens a socket, so single-player stays single-player.
#pragma once

namespace mgmp {

// Reads the saved name and last IP from mgmp_lobby.txt beside the DLL. Cheap,
// no ImGui. From ui_init.
void lobby_init();

// From mgmp_ui's bring-up, right after the ImGui context exists and before the
// first frame: loads the Windows fonts the lobby draws with. Falls back to
// ImGui's built-in font if they are missing, so it cannot fail.
void lobby_setup_fonts();

// Whether the lobby has anything to draw this frame -- on the title screen, in
// a session, or with its window open. mgmp_ui runs an ImGui frame when this OR
// the debug panel wants one. Polls the scene list a few times a second, not
// every frame.
bool lobby_wants_frame();

// Inside an ImGui frame (between NewFrame and Render).
void lobby_draw();

} // namespace mgmp
