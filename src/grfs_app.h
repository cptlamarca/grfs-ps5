// Ghost Recon: Future Soldier - native PS5 port (ReXGlue runtime's PS5 layer).
#pragma once

#include <rex/rex_app.h>

class GrfsApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GrfsApp>(new GrfsApp(ctx, "grfs", PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPostLoadXexImage() override;
};
