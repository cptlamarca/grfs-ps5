// The "help keep me active" screen, shown from the main menu instead of the
// downloadable content: a title, a text and a QR code of a link (to support
// the project), from the news server (its /support.txt) or the support_*
// options.

#pragma once

#include <cstdint>

#include <rex/ui/imgui_dialog.h>

struct ImFont;

namespace rex {
class ReXApp;
class Runtime;
}  // namespace rex

class SupportScreen : public rex::ui::ImGuiDialog {
 public:
  SupportScreen(rex::ReXApp* app, rex::ui::ImGuiDrawer* imgui_drawer, rex::Runtime* runtime,
                ImFont* font, ImFont* title_font);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  rex::ReXApp* app_;
  rex::Runtime* runtime_;
  ImFont* font_;
  ImFont* title_font_;

  bool pad_released_ = false;
  bool closing_ = false;
  uint16_t previous_buttons_ = 0;
};
