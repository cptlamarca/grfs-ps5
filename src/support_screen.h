// Screens shown from the main menu instead of the downloadable content and
// the Ghost Feed's multiplayer challenges: "help keep me active" (a title, a
// text and a QR code of a link, to support the project) and the donors. Their
// contents come from the news server (its /support.txt and /donors.txt), the
// first one's also from the support_* options.

#pragma once

#include <cstdint>

#include <rex/ui/imgui_dialog.h>

struct ImFont;

namespace rex {
class ReXApp;
class Runtime;
}  // namespace rex

// A full screen overlay closed with Circle or Back.
class OverlayScreen : public rex::ui::ImGuiDialog {
 protected:
  OverlayScreen(rex::ReXApp* app, rex::ui::ImGuiDrawer* imgui_drawer, rex::Runtime* runtime,
                ImFont* font, ImFont* title_font)
      : ImGuiDialog(imgui_drawer),
        app_(app),
        runtime_(runtime),
        font_(font),
        title_font_(title_font) {}

  // Buttons newly pressed (the left stick as the d-pad), nothing until every
  // button held when the screen opened (the one that opened it) is released.
  uint16_t PollPad();
  // Whether the screen has asked to be hidden, and shouldn't be drawn.
  bool closed() const { return hidden_; }

  rex::ReXApp* app_;
  rex::Runtime* runtime_;
  ImFont* font_;
  ImFont* title_font_;

 private:
  bool pad_released_ = false;
  bool closing_ = false;
  bool hidden_ = false;
  uint16_t previous_buttons_ = 0;
};

class SupportScreen : public OverlayScreen {
 public:
  SupportScreen(rex::ReXApp* app, rex::ui::ImGuiDrawer* imgui_drawer, rex::Runtime* runtime,
                ImFont* font, ImFont* title_font);

 protected:
  void OnDraw(ImGuiIO& io) override;
};

class DonorsScreen : public OverlayScreen {
 public:
  DonorsScreen(rex::ReXApp* app, rex::ui::ImGuiDrawer* imgui_drawer, rex::Runtime* runtime,
               ImFont* font, ImFont* title_font);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  int selected_ = 0;
  int scroll_to_ = -1;
};
