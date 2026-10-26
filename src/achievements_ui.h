// Achievements screen driven by the pad, shown from the main menu.

#pragma once

#include <cstdint>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include <rex/system/achievement_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/achievement_icon_cache.h>
#include <rex/ui/overlay/achievement_notification.h>

struct ImFont;

namespace rex {
class ReXApp;
class Runtime;
}  // namespace rex

// Whether the game runs in Portuguese, for the texts and the achievements.
bool IsPortuguese();

class AchievementsScreen : public rex::ui::ImGuiDialog {
 public:
  AchievementsScreen(rex::ReXApp* app, rex::ui::ImGuiDrawer* imgui_drawer,
                     rex::ui::ImmediateDrawer* immediate_drawer, rex::Runtime* runtime,
                     rex::system::AchievementManager* achievements, ImFont* font,
                     ImFont* title_font);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Buttons newly pressed since the last frame; nothing until every button
  // held when the screen opened (the one that opened it) is released.
  uint16_t PollPad();

  rex::ReXApp* app_;
  rex::Runtime* runtime_;
  rex::system::AchievementManager* achievements_;
  rex::ui::AchievementIconCache icon_cache_;
  ImFont* font_;
  ImFont* title_font_;

  bool pad_released_ = false;
  bool closing_ = false;
  uint16_t previous_buttons_ = 0;
  int selected_ = 0;
  double repeat_time_ = 0.0;

  // The skull art behind it, and the animations: time since shown and the
  // list's scroll, easing to the focused trophy.
  std::unique_ptr<rex::ui::ImmediateTexture> background_;
  float background_aspect_ = 1.6f;
  double shown_at_ = -1.0;
  float scroll_ = 0.0f;
};

// Unlock notification, like the Xbox 360's, big enough for a TV.
class AchievementToast : public rex::ui::AchievementNotificationDialog {
 public:
  AchievementToast(rex::ui::ImGuiDrawer* imgui_drawer, rex::ui::ImmediateDrawer* immediate_drawer,
                   rex::Runtime* runtime, ImFont* font, ImFont* title_font);

  // From any thread.
  void Push(const rex::system::AchievementEvent& event) override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  static constexpr float kDisplaySeconds = 6.0f;

  std::mutex mutex_;
  std::deque<rex::system::AchievementInfo> queue_;
  std::optional<std::chrono::steady_clock::time_point> shown_since_;
  rex::ui::AchievementIconCache icon_cache_;
  ImFont* font_;
  ImFont* title_font_;
};
