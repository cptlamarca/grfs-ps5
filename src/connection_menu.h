#pragma once

#include <cstdint>

#include <rex/system/xmemory.h>

// Patches the multiplayer menu's CONNECTION choices (Local Network, Online
// (VPS)) in Multiplayer.feu once it's loaded (done by the hook queueing its
// commands).
void PatchConnectionMenu(rex::memory::Memory* memory);

// Before the game gets RequestChangeConnectionType: its argument (a guest
// string) is the connection type, or the Local Network's text ID, which
// becomes the LAN's type.
void OnConnectionRequest(uint8_t* base, uint32_t type_address);
