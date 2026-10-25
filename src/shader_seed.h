// The shaders and pipelines shipped with the port (the app's shaders/ folder,
// the shareable shader storage of earlier play), added to the player's own
// storage before the runtime loads it, so they're compiled at startup rather
// than in the middle of play. What the player's storage already has is kept.

#pragma once

#include <filesystem>

void MergeShaderSeed(const std::filesystem::path& seed_folder,
                     const std::filesystem::path& cache_root);

// The shaders shared by every console through the news server (its
// /shaders/NAME, next to news_url), of the format of the app's own
// (seed_folder): downloaded into the storage's remote/ folder before it's
// merged (MergeShaderSeed of that folder), waiting a few seconds at most.
void FetchSharedShaders(const std::filesystem::path& seed_folder,
                        const std::filesystem::path& cache_root);

// Sends this console's storage there every 10 minutes when it grew, with its
// console ID.
void StartSharedShaders(const std::filesystem::path& seed_folder,
                        const std::filesystem::path& cache_root);
