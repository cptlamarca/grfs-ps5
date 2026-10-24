// The PS5's console, known to the news server (tools/news_server.py) by its
// console ID (OpenPSID; the hardware serial number isn't available to games): registered when the game starts, its class XP kept there too
// (the server's copy wins, the file's at least), and bannable from the online
// (System Link through the relay, which gets the serial number too). Nothing
// on other platforms.

#pragma once

#include <string>

namespace rex {
class Runtime;
}  // namespace rex

// From the app, before the game starts.
void StartPlayerAccount(rex::Runtime* runtime);

// Whether this console is banned from the online, and why.
bool PlayerBanned(std::string* reason = nullptr);
