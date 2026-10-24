// The port's texts (PlayStation names, Local Network, Online (VPS), the donors
// and support entries...) in the game's Language/Oasis_*.inf files, applied to
// the player's own files at start (their originals kept as .orig): the port
// doesn't distribute the game's files.

#pragma once

#include <filesystem>

void PatchLanguageFiles(const std::filesystem::path& game_data_root);
