// See support_screen.h. /support.txt (tools/news_server.py) has the title on
// its first line, the link on its second and the text after them.

#include "support_screen.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "achievements_ui.h"
#include "news_hooks.h"
#include "third_party/qrcodegen/qrcodegen.hpp"

REXCVAR_DEFINE_STRING(support_title, "", "GRFS",
                      "Title of the main menu's \"help keep me active\" screen (the news server's "
                      "/support.txt replaces it)");
REXCVAR_DEFINE_STRING(support_text, "", "GRFS",
                      "Text of the \"help keep me active\" screen (\\n: a new line)");
REXCVAR_DEFINE_STRING(support_url, "", "GRFS",
                      "Link of the \"help keep me active\" screen, shown as a QR code");

namespace {

constexpr float kFontScaleAt1080 = 0.5f;
constexpr ImU32 kBackgroundTop = IM_COL32(16, 30, 54, 255);
constexpr ImU32 kBackgroundBottom = IM_COL32(4, 8, 16, 255);
constexpr ImVec4 kText = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
constexpr ImVec4 kDimText = ImVec4(0.70f, 0.73f, 0.78f, 1.0f);

struct Content {
  std::string title, text, url;
};

std::mutex content_mutex;
bool content_fetching = false, content_fetched = false;
Content content;

std::string Unescape(std::string text) {
  for (size_t at; (at = text.find("\\n")) != std::string::npos;) {
    text.replace(at, 2, "\n");
  }
  return text;
}

Content DefaultContent() {
  Content defaults;
  defaults.title = REXCVAR_GET(support_title);
  defaults.text = Unescape(REXCVAR_GET(support_text));
  defaults.url = REXCVAR_GET(support_url);
  if (defaults.title.empty()) {
    defaults.title = IsPortuguese() ? "Ajude a me manter ativo" : "Help keep me active";
  }
  return defaults;
}

// The content; with refresh, downloads it again from the news server (next
// to news_url's file) in the background.
Content GetContent(bool refresh) {
  std::lock_guard<std::mutex> lock(content_mutex);
  if (!content_fetched) {
    content = DefaultContent();
    content_fetched = true;
  }
  std::string news = rex::cvar::Query<std::string>("news_url");
  size_t slash = news.rfind('/');
  if (refresh && !content_fetching && slash != std::string::npos && slash > 7) {
    content_fetching = true;
    std::thread([url = news.substr(0, slash) + "/support.txt"] {
      std::string body;
      bool ok = HttpGet(url, body);
      std::lock_guard<std::mutex> lock(content_mutex);
      content_fetching = false;
      size_t first = body.find('\n');
      if (!ok || first == std::string::npos) {
        return;
      }
      size_t second = body.find('\n', first + 1);
      Content fetched;
      fetched.title = body.substr(0, first);
      fetched.url = body.substr(first + 1, second == std::string::npos ? std::string::npos
                                                                         : second - first - 1);
      fetched.text = second == std::string::npos ? "" : body.substr(second + 1);
      for (std::string* field : {&fetched.title, &fetched.url}) {
        field->erase(std::remove(field->begin(), field->end(), '\r'), field->end());
      }
      fetched.text.erase(std::remove(fetched.text.begin(), fetched.text.end(), '\r'),
                         fetched.text.end());
      if (!fetched.title.empty()) {
        content = fetched;
      }
    }).detach();
  }
  return content;
}

void DrawCircleButton(ImDrawList* draw, ImVec2 center, float radius) {
  draw->AddCircleFilled(center, radius, IM_COL32(255, 255, 255, 235), 24);
  draw->AddCircle(center, radius * 0.5f, IM_COL32(20, 24, 32, 255), 24, radius * 0.17f);
}

}  // namespace

SupportScreen::SupportScreen(rex::ReXApp* app, rex::ui::ImGuiDrawer* imgui_drawer,
                             rex::Runtime* runtime, ImFont* font, ImFont* title_font)
    : ImGuiDialog(imgui_drawer), app_(app), runtime_(runtime), font_(font), title_font_(title_font) {
  GetContent(true);
}

void SupportScreen::OnDraw(ImGuiIO& io) {
  // The pad: Circle or Back closes it, once the button that opened it is up,
  // and hides it once that one is up too, so the game doesn't get it.
  auto* input = static_cast<rex::input::InputSystem*>(runtime_->input_system());
  rex::input::X_INPUT_STATE state = {};
  if (!input || input->GetRawState(0, &state) != 0 /* X_ERROR_SUCCESS */) {
    state = {};
  }
  uint16_t buttons = state.gamepad.buttons;
  uint16_t pressed = pad_released_ ? uint16_t(buttons & ~previous_buttons_) : 0;
  pad_released_ = pad_released_ || !buttons;
  previous_buttons_ = buttons;
  if (closing_ && !buttons) {
    app_->SetAchievementsOverlayVisible(false);
    return;
  }
  if (pressed & (rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_BACK)) {
    closing_ = true;
  }

  const Content shown = GetContent(false);
  const ImVec2 display = io.DisplaySize;
  const float scale = display.y / 1080.0f;
  const float font_scale = scale * kFontScaleAt1080;

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##grfs_support", nullptr, kFlags)) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(ImVec2(0, 0), display, kBackgroundTop, kBackgroundTop,
                                  kBackgroundBottom, kBackgroundBottom);
    if (font_) {
      ImGui::PushFont(font_);
    }
    const float left = display.x * 0.10f;
    const float qr_size = display.y * 0.46f;
    const float qr_x = display.x * 0.90f - qr_size;
    const float text_width = qr_x - left - 80.0f * scale;

    // The title and the text, left; the QR code and its link, right.
    if (title_font_) {
      ImGui::PushFont(title_font_);
    }
    ImGui::SetWindowFontScale(font_scale * 0.9f);
    ImGui::SetCursorPos(ImVec2(left, display.y * 0.16f));
    ImGui::PushTextWrapPos(left + text_width);
    ImGui::TextColored(kText, "%s", shown.title.c_str());
    ImGui::PopTextWrapPos();
    if (title_font_) {
      ImGui::PopFont();
    }
    ImGui::SetWindowFontScale(font_scale);
    ImGui::SetCursorPos(ImVec2(left, ImGui::GetCursorPosY() + 30.0f * scale));
    ImGui::PushTextWrapPos(left + text_width);
    ImGui::TextColored(kDimText, "%s", shown.text.c_str());
    ImGui::PopTextWrapPos();

    if (!shown.url.empty()) {
      const float qr_y = display.y * 0.16f;
      try {
        qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(shown.url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        // A white card with a quiet zone of 3 modules.
        const int size = qr.getSize();
        const float module = qr_size / float(size + 6);
        draw->AddRectFilled(ImVec2(qr_x, qr_y), ImVec2(qr_x + qr_size, qr_y + qr_size),
                            IM_COL32(255, 255, 255, 255), 12.0f * scale);
        for (int y = 0; y < size; ++y) {
          for (int x = 0; x < size; ++x) {
            if (qr.getModule(x, y)) {
              ImVec2 corner(qr_x + (x + 3) * module, qr_y + (y + 3) * module);
              draw->AddRectFilled(corner, ImVec2(corner.x + module + 0.5f, corner.y + module + 0.5f),
                                  IM_COL32(0, 0, 0, 255));
            }
          }
        }
      } catch (const std::exception&) {
        // (a link too long for a QR code: only its text)
      }
      ImGui::SetWindowFontScale(font_scale * 0.8f);
      ImVec2 url_size = ImGui::CalcTextSize(shown.url.c_str(), nullptr, false, qr_size);
      ImGui::SetCursorPos(ImVec2(qr_x + std::max(0.0f, (qr_size - url_size.x) / 2.0f),
                                 qr_y + qr_size + 24.0f * scale));
      ImGui::PushTextWrapPos(qr_x + qr_size);
      ImGui::TextColored(kDimText, "%s", shown.url.c_str());
      ImGui::PopTextWrapPos();
    }

    // The footer: Circle, back.
    ImGui::SetWindowFontScale(font_scale);
    const char* back = IsPortuguese() ? "Voltar" : "Back";
    const float hint_y = display.y - 70.0f * scale;
    const float radius = 18.0f * scale;
    const float back_width = ImGui::CalcTextSize(back).x;
    const float hint_x = display.x * 0.90f - back_width - radius * 2.0f - 12.0f * scale;
    DrawCircleButton(draw, ImVec2(hint_x + radius, hint_y + radius), radius);
    ImGui::SetCursorPos(ImVec2(hint_x + radius * 2.0f + 12.0f * scale, hint_y));
    ImGui::TextColored(kText, "%s", back);
    if (font_) {
      ImGui::PopFont();
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}
