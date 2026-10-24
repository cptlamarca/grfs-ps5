// The DualSense's own features on the PS5: adaptive triggers during play (R2
// a gun's trigger, L2 a light resistance to aim) and the light bar (the
// Ghosts' blue, brighter in play, dim when paused, flashing red with a
// strong rumble: hits and explosions). Nothing on other platforms.

#pragma once

#include <string>

// A command of the game's Flash screens (src/connection_menu.cpp sees them
// all): a match launched, the pause's grid shown, a menu screen loaded.
void DualSenseUiCommand(const std::string& command, const std::string& value);

// Whether the game shows its menus (no match or mission running).
bool InMenus();
