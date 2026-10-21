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

REXCVAR_DECLARE(uint32_t, user_language);

#if defined(__PROSPERO__)
extern "C" int sceSystemServiceParamGetInt(int param_id, int* value);
#endif

namespace {

#if defined(__PROSPERO__)
// The console's language (SCE_SYSTEM_SERVICE_PARAM_ID_LANG) as an Xbox 360
// language ID (XLanguage), for the game's text and voices.
uint32_t ConsoleLanguage() {
  constexpr int kParamIdLang = 1;
  int lang = 1;
  if (sceSystemServiceParamGetInt(kParamIdLang, &lang) != 0) {
    return 1;
  }
  switch (lang) {
    case 0: return 2;             // Japanese
    case 2: case 22: return 4;    // French, French (Canada)
    case 3: case 20: return 5;    // Spanish, Spanish (Latin America)
    case 4: return 3;             // German
    case 5: return 6;             // Italian
    case 7: case 17: return 9;    // Portuguese, Portuguese (Brazil)
    case 8: return 12;            // Russian
    case 9: return 7;             // Korean
    case 10: return 8;            // Chinese (Traditional)
    case 11: return 10;           // Chinese (Simplified)
    case 16: return 11;           // Polish
    default: return 1;            // English and the rest
  }
}
#endif

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

#if defined(__PROSPERO__)
  // The console's language, unless grfs.toml sets user_language (read after).
  rex::cvar::SetFlagByName("user_language", std::to_string(ConsoleLanguage()));
#endif

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
  REXLOG_INFO("language: {}", REXCVAR_GET(user_language));
  REXLOG_INFO("game folder: {}", game_data_root().string());
}
