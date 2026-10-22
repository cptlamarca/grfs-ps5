// Ghost Recon: Future Soldier - native PS5 port (ReXGlue runtime's PS5 layer).
#pragma once

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

  // The game folder, for the UI.
  std::filesystem::path GameFolder() const { return game_data_root(); }

  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPostLoadXexImage() override;

 protected:
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override;
  void OnPreLaunchModule() override;

 private:
  static inline GrfsApp* instance_ = nullptr;
  ImFont* font_ = nullptr;
  ImFont* title_font_ = nullptr;
};
