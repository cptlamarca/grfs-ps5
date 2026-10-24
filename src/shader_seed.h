// The shaders and pipelines shipped with the port (the app's shaders/ folder,
// the shareable shader storage of earlier play), added to the player's own
// storage before the runtime loads it, so they're compiled at startup rather
// than in the middle of play. What the player's storage already has is kept.

#pragma once

#include <filesystem>

void MergeShaderSeed(const std::filesystem::path& seed_folder,
                     const std::filesystem::path& cache_root);
