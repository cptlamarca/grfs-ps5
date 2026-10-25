// The screen shown before the game when its disc files are missing: where
// they go, the disc images (.iso) found on the console and USB drives, one
// of which is extracted there on Cross, then the game starts.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include "disc_image.h"

struct ImFont;

class IsoScreen : public rex::ui::ImGuiDialog {
 public:
  // `game_folder` gets the files; `done` runs (on the UI thread, from a draw)
  // once they're there.
  IsoScreen(rex::ui::ImGuiDrawer* imgui_drawer, rex::ui::ImmediateDrawer* immediate_drawer,
            ImFont* font, ImFont* title_font, std::filesystem::path game_folder,
            std::function<void()> done);
  ~IsoScreen() override;

  // Whether `folder` has the game's files.
  static bool HasGameFiles(const std::filesystem::path& folder);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class State { kChoosing, kExtracting, kFailed, kDone };
  struct Image {
    std::filesystem::path path;
    uint64_t size = 0;
  };

  void Search();
  void Extract(const Image& image);
  uint32_t PollPad();

  // The wallpaper: the game's skull (assets/iso_background.png, in the app).
  std::unique_ptr<rex::ui::ImmediateTexture> background_;
  float background_aspect_ = 1.6f;
  ImFont* font_;
  ImFont* title_font_;
  std::filesystem::path game_folder_;
  std::function<void()> done_;

  std::vector<Image> images_;
  int selected_ = 0;

  std::atomic<State> state_{State::kChoosing};
  std::thread worker_;
  ExtractProgress progress_;
  std::atomic<uint64_t> total_{0};
  std::mutex mutex_;
  std::string current_file_, error_;

  int32_t pad_ = -1;
  uint32_t previous_buttons_ = 0;
  bool done_called_ = false;

  // Animations: time since shown, since the state last changed, the list's
  // highlight (sliding to the selected row) and the progress bar.
  double shown_at_ = -1.0, state_at_ = 0.0;
  State drawn_state_ = State::kChoosing;
  float highlight_row_ = 0.0f;
  float shown_progress_ = 0.0f;
};
