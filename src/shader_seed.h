// The shaders and pipelines shipped with the port (the app's shaders/ folder,
// the shareable shader storage of earlier play), added to the player's own
// storage before the runtime loads it, so they're compiled at startup rather
// than in the middle of play. What the player's storage already has is kept.

#pragma once

#include <filesystem>

void MergeShaderSeed(const std::filesystem::path& seed_folder,
                     const std::filesystem::path& cache_root);

// The shaders shared by every console through the news server (its
// /shaders/NAME, next to news_url): downloaded in the background into the
// storage's remote/ folder, merged at the next start (MergeShaderSeed of
// that folder), when they're of the format of the app's own (seed_folder);
// this console's storage is sent there every 10 minutes when it grew, with
// its console ID.
void StartSharedShaders(const std::filesystem::path& seed_folder,
                        const std::filesystem::path& cache_root);
