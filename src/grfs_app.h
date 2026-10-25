// Ghost Recon: Future Soldier - native PS5 port (ReXGlue runtime's PS5 layer).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <string>

#include <rex/rex_app.h>

struct ImFont;

class GrfsApp : public rex::ReXApp {
 public:
  GrfsApp(rex::ui::WindowedAppContext& ctx, std::string_view name, rex::PPCImageInfo ppc_info)
      : rex::ReXApp(ctx, name, ppc_info) {
    instance_ = this;
  }

  // In grfs_app.cpp, which has the generated image config.
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

  // For game patches, which run on guest threads.
  static GrfsApp* instance() { return instance_; }

  // The overlay screens, from the main menu: the trophies, the "help keep me
  // active" one or the donors.
  void ShowTrophies();
  void ShowSupport();
  void ShowDonors();
  // A notice (title and text), from any thread.
  void ShowNotice(std::string title, std::string text);
  // The System Link decision point waiting for a choice, if there is one.
  void ShowDecision();

  // The game folder, for the UI.
  std::filesystem::path GameFolder() const { return game_data_root(); }

  void OnConfigurePaths(rex::PathConfig& paths) override;
  // Without the game's disc files, a screen to extract them from an ISO
  // first (src/iso_screen.cpp).
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override;
  void OnPostLoadXexImage() override;

 protected:
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override;
  void OnPreLaunchModule() override;

 private:
  static inline GrfsApp* instance_ = nullptr;
  std::unique_ptr<rex::ui::ImGuiDialog> iso_screen_;
  enum class Overlay { kTrophies, kSupport, kDonors, kNotice, kDecision };
  std::atomic<Overlay> overlay_{Overlay::kTrophies};
  std::mutex notice_mutex_;
  std::string notice_title_, notice_text_;
  ImFont* font_ = nullptr;
  ImFont* title_font_ = nullptr;
};
