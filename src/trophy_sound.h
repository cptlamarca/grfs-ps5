// The sound played with the trophy notification.
#pragma once

#include <filesystem>

// Plays metadata/trophy.wav from the game folder when there is one (16-bit
// PCM, mono or stereo, any rate), else a chime of its own. Doesn't wait.
void PlayTrophySound(const std::filesystem::path& game_data_root);
