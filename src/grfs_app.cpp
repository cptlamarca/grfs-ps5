// Ghost Recon: Future Soldier - native PS5 port: where things are, and the
// settings the game needs from the runtime.
#include "generated/default/grfs_init.h"
#include "grfs_app.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <imgui.h>

#include "achievements_ui.h"
#include "language_patch.h"
#include "player_account.h"
#include "shader_seed.h"
#include "support_screen.h"

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
    "vulkan_sparse_shared_memory = false\n"
    "# (one Vulkan submission per frame: mid-frame vkQueueSubmit stalls for ms on the PS5)\n"
    "vulkan_submit_on_primary_buffer_end = false\n"
    "# (GPU copies of memory kept between frames: 16 KB host pages watched by all their guest pages)\n"
    "clear_memory_page_state = false\n"
    "# (AMD FSR 1: sharper upscaling of 2080x1200 to the 4K screen)\n"
    "present_effect = \"fsr\"\n";

// Settings tests on the console, one per start: each line of tests.txt (beside
// the app) is a list of settings ("key = value", separated by ';'); each start
// takes the next line (tests_next.txt counts) and writes them into grfs.toml.
// What the start tested, for the log.
std::string test_profile;

void ApplyNextTestProfile(const std::filesystem::path& app, const std::filesystem::path& config) {
  std::ifstream list(app / "tests.txt");
  std::vector<std::string> profiles;
  for (std::string line; std::getline(list, line);) {
    if (!line.empty() && line[0] != '#') profiles.push_back(line);
  }
  if (profiles.empty()) return;
  size_t next = 0;
  std::ifstream(app / "tests_next.txt") >> next;
  const size_t index = next % profiles.size();
  std::ofstream(app / "tests_next.txt", std::ios::trunc) << index + 1 << "\n";
  test_profile = std::to_string(index + 1) + "/" + std::to_string(profiles.size()) + ": " +
                 profiles[index];

  // The defaults, the profile's keys replaced.
  std::vector<std::pair<std::string, std::string>> settings;
  std::string item;
  for (std::istringstream items(profiles[index]); std::getline(items, item, ';');) {
    const size_t eq = item.find('=');
    if (eq == std::string::npos) continue;
    auto trim = [](std::string s) {
      s.erase(0, s.find_first_not_of(" \t"));
      s.erase(s.find_last_not_of(" \t\r") + 1);
      return s;
    };
    settings.emplace_back(trim(item.substr(0, eq)), trim(item.substr(eq + 1)));
  }
  std::ostringstream out;
  std::istringstream defaults(kDefaultConfig);
  for (std::string line; std::getline(defaults, line);) {
    bool replaced = false;
    for (const auto& [key, value] : settings) {
      replaced |= line.rfind(key + " ", 0) == 0 || line.rfind(key + "=", 0) == 0;
    }
    if (!replaced) out << line << "\n";
  }
  out << "# test " << test_profile << "\n";
  for (const auto& [key, value] : settings) out << key << " = " << value << "\n";
  std::ofstream(config, std::ios::trunc) << out.str();
}

// The port's own files (fonts, translations): the app's metadata folder, as
// released, or the one of the game folder (where earlier builds had them).
std::filesystem::path MetadataFolder(const std::filesystem::path& game_data_root) {
  std::error_code ec;
  std::filesystem::path app = rex::filesystem::GetExecutableFolder() / "metadata";
  return std::filesystem::exists(app, ec) ? app : game_data_root / "metadata";
}

}  // namespace

void GrfsApp::OnConfigurePaths(rex::PathConfig& paths) {
  const std::filesystem::path app = rex::filesystem::GetExecutableFolder();
  std::error_code ec;
  // The disc files: <app>/Assets (where the app store extracts the disc image)
  // or <app>/assets (extracted from the ISO by hand, default.xex inside).
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
  ApplyNextTestProfile(app, paths.config_path);
}

void GrfsApp::OnPostLoadXexImage() {
  REXLOG_INFO("==== Ghost Recon: Future Soldier PS5 port (test build) ====");
  REXLOG_INFO("language: {}", REXCVAR_GET(user_language));
  if (!test_profile.empty()) REXLOG_INFO("settings test {}", test_profile);
  REXLOG_INFO("game folder: {}", game_data_root().string());
}

std::unique_ptr<rex::ui::WindowedApp> GrfsApp::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<GrfsApp>(new GrfsApp(ctx, "grfs", PPCImageConfig));
}

void GrfsApp::OnConfigureFonts(ImFontAtlas* atlas) {
  // Noto Sans (SIL Open Font License, metadata/fonts/OFL.txt) at 2160p sizes
  // for the achievements screen: the default font has no accented letters
  // big enough for a TV.
  static const ImWchar kLatin[] = {0x0020, 0x00FF, 0x2018, 0x201D, 0};
  auto load = [&](const char* file, float size) -> ImFont* {
    std::filesystem::path path = MetadataFolder(game_data_root()) / "fonts" / file;
    if (!std::filesystem::exists(path)) {
      REXLOG_WARN("Font {} missing", path.string());
      return nullptr;
    }
    ImFontConfig config;
    config.OversampleH = config.OversampleV = 2;
    return atlas->AddFontFromFileTTF(path.string().c_str(), size, &config, kLatin);
  };
  font_ = load("NotoSans-Regular.ttf", 56.0f);
  title_font_ = load("NotoSans-Bold.ttf", 88.0f);
}

void GrfsApp::ShowTrophies() {
  overlay_ = Overlay::kTrophies;
  SetAchievementsOverlayVisible(true);
}

void GrfsApp::ShowSupport() {
  overlay_ = Overlay::kSupport;
  SetAchievementsOverlayVisible(true);
}

void GrfsApp::ShowDonors() {
  overlay_ = Overlay::kDonors;
  SetAchievementsOverlayVisible(true);
}

void GrfsApp::ShowNotice(std::string title, std::string text) {
  {
    std::lock_guard<std::mutex> lock(notice_mutex_);
    notice_title_ = std::move(title);
    notice_text_ = std::move(text);
  }
  overlay_ = Overlay::kNotice;
  SetAchievementsOverlayVisible(true);
}

void GrfsApp::ShowDecision() {
  if (DecisionScreen::Waiting()) {
    overlay_ = Overlay::kDecision;
    SetAchievementsOverlayVisible(true);
  }
}

std::unique_ptr<rex::ui::ImGuiDialog> GrfsApp::CreateAchievementsOverlay() {
  if (!imgui_drawer() || !runtime() || !runtime()->kernel_state()) {
    return nullptr;
  }
  if (overlay_ == Overlay::kSupport) {
    return std::make_unique<SupportScreen>(this, imgui_drawer(), runtime(), font_, title_font_);
  }
  if (overlay_ == Overlay::kDonors) {
    return std::make_unique<DonorsScreen>(this, imgui_drawer(), runtime(), font_, title_font_);
  }
  if (overlay_ == Overlay::kDecision) {
    return std::make_unique<DecisionScreen>(this, imgui_drawer(), runtime(), font_, title_font_,
                                            game_data_root());
  }
  if (overlay_ == Overlay::kNotice) {
    std::lock_guard<std::mutex> lock(notice_mutex_);
    return std::make_unique<NoticeScreen>(this, imgui_drawer(), runtime(), font_, title_font_,
                                          notice_title_, notice_text_);
  }
  return std::make_unique<AchievementsScreen>(this, imgui_drawer(), immediate_drawer(), runtime(),
                                              &achievements(), font_, title_font_);
}

std::unique_ptr<rex::ui::AchievementNotificationDialog>
GrfsApp::CreateAchievementNotificationDialog() {
  if (!imgui_drawer() || !runtime()) {
    return nullptr;
  }
  return std::make_unique<AchievementToast>(imgui_drawer(), immediate_drawer(), runtime(), font_,
                                            title_font_);
}

void GrfsApp::OnPreLaunchModule() {
  PatchLanguageFiles(game_data_root());
  const std::filesystem::path seed = rex::filesystem::GetExecutableFolder() / "shaders";
  MergeShaderSeed(seed, runtime()->cache_root());
  MergeShaderSeed(runtime()->cache_root() / "shaders" / "remote", runtime()->cache_root());
  StartSharedShaders(seed, runtime()->cache_root());
  StartPlayerAccount(runtime());
  // The XEX only has the achievements in English.
  if (IsPortuguese()) {
    achievements().LoadMetadataFile(MetadataFolder(game_data_root()) / "achievements_pt.toml");
  }
}
