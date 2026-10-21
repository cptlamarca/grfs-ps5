// Ghost Recon: Future Soldier - native PS5 port: where things are, and the
// settings the game needs from the runtime.
#include "generated/default/grfs_init.h"
#include "grfs_app.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

namespace {

// The settings a first start writes (grfs.toml beside the app; edit it over
// FTP). Bump the version to replace an older file.
constexpr char kConfigVersion[] = "(settings v1)";
constexpr char kDefaultConfig[] =
    "# Ghost Recon: Future Soldier - settings (settings v1)\n"
    "gpu_plugin = \"xenos\"\n"
    "input_backend = \"xinput\"\n"
    "# 1 = the Xbox 360's 1040x600, 2 = 2080x1200, 3 = 3120x1800\n"
    "resolution_scale = 2\n"
    "# EDRAM emulated per pixel: the other path flashes black squares on characters\n"
    "render_target_path_vulkan = \"fsi\"\n"
    "gpu_allow_invalid_fetch_constants = true\n"
    "vsync = true\n"
    "# (a window, not fullscreen: fullscreen gives overlays a 4:3 screen on the PS5)\n"
    "fullscreen = false\n"
    "# (the PS5's GPU driver: one plain buffer for the Xbox 360's memory, no sparse binding)\n"
    "vulkan_sparse_shared_memory = false\n";

}  // namespace

void GrfsApp::OnConfigurePaths(rex::PathConfig& paths) {
  const std::filesystem::path app = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  // The disc files: <app>/assets (extracted from the ISO, default.xex inside).
  if (paths.game_data_root.empty()) {
    paths.game_data_root = std::filesystem::exists(app / "Assets" / "default.xex", ec)
                               ? app / "Assets"
                               : app / "assets";
  }
  paths.user_data_root = app / "UserData";
  paths.cache_root = app / "UserData" / "cache";

  // DualSense button prompts and high-resolution textures, if installed.
  if (std::filesystem::is_directory(app / "textures", ec)) {
    rex::cvar::SetFlagByName("texture_replacement_path", (app / "textures").string());
  }

  bool current = false;
  {
    std::ifstream in(paths.config_path);
    std::string first;
    std::getline(in, first);
    current = first.find(kConfigVersion) != std::string::npos;
  }
  if (!current) {
    std::ofstream(paths.config_path, std::ios::trunc) << kDefaultConfig;
  }
}

void GrfsApp::OnPostLoadXexImage() {
  REXLOG_INFO("==== Ghost Recon: Future Soldier PS5 port (test build) ====");
  REXLOG_INFO("game folder: {}", game_data_root().string());
}
