// See support_screen.h. /support.txt (tools/news_server.py) has the title on
// its first line, the link on its second and the text after them;
// /donors.txt a donor per line: the name, a tab and a message.

#include "support_screen.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

struct Donor {
  std::string name, message;
};

std::mutex donors_mutex;
bool donors_fetching = false;
std::vector<Donor> donors;

// The donors; with refresh, downloads them again in the background.
std::vector<Donor> GetDonors(bool refresh) {
  std::lock_guard<std::mutex> lock(donors_mutex);
  std::string news = rex::cvar::Query<std::string>("news_url");
  size_t slash = news.rfind('/');
  if (refresh && !donors_fetching && slash != std::string::npos && slash > 7) {
    donors_fetching = true;
    std::thread([url = news.substr(0, slash) + "/donors.txt"] {
      std::string body;
      bool ok = HttpGet(url, body);
      std::vector<Donor> fetched;
      for (size_t start = 0; ok && start < body.size();) {
        size_t end = body.find('\n', start);
        std::string line = body.substr(start, end == std::string::npos ? end : end - start);
        start = end == std::string::npos ? body.size() : end + 1;
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        size_t tab = line.find('\t');
        Donor donor{line.substr(0, tab), tab == std::string::npos ? "" : line.substr(tab + 1)};
        if (!donor.name.empty()) {
          fetched.push_back(std::move(donor));
        }
      }
      std::lock_guard<std::mutex> lock(donors_mutex);
      donors_fetching = false;
      if (ok) {
        donors = std::move(fetched);
      }
    }).detach();
  }
  return donors;
}

void DrawCircleButton(ImDrawList* draw, ImVec2 center, float radius) {
  draw->AddCircleFilled(center, radius, IM_COL32(255, 255, 255, 235), 24);
  draw->AddCircle(center, radius * 0.5f, IM_COL32(20, 24, 32, 255), 24, radius * 0.17f);
}

// The Circle "Back" hint, bottom right.
void DrawBackHint(ImDrawList* draw, ImVec2 display, float scale) {
  const char* back = IsPortuguese() ? "Voltar" : "Back";
  const float hint_y = display.y - 70.0f * scale;
  const float radius = 18.0f * scale;
  const float hint_x =
      display.x * 0.90f - ImGui::CalcTextSize(back).x - radius * 2.0f - 12.0f * scale;
  DrawCircleButton(draw, ImVec2(hint_x + radius, hint_y + radius), radius);
  ImGui::SetCursorPos(ImVec2(hint_x + radius * 2.0f + 12.0f * scale, hint_y));
  ImGui::TextColored(kText, "%s", back);
}

}  // namespace

uint16_t OverlayScreen::PollPad() {
  // Nothing until the button that opened the screen is up; Circle or Back
  // closes it, hidden once that one is up too, so the game doesn't get it.
  auto* input = static_cast<rex::input::InputSystem*>(runtime_->input_system());
  rex::input::X_INPUT_STATE state = {};
  if (!input || input->GetRawState(0, &state) != 0 /* X_ERROR_SUCCESS */) {
    state = {};
  }
  uint16_t buttons = state.gamepad.buttons;
  if (state.gamepad.thumb_ly > 16000) {
    buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_UP;
  } else if (state.gamepad.thumb_ly < -16000) {
    buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
  }
  uint16_t pressed = pad_released_ ? uint16_t(buttons & ~previous_buttons_) : 0;
  pad_released_ = pad_released_ || !buttons;
  previous_buttons_ = buttons;
  if (closing_ && !buttons) {
    hidden_ = true;
    app_->SetAchievementsOverlayVisible(false);
    return 0;
  }
  if (pressed & (rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_BACK)) {
    closing_ = true;
  }
  return pressed;
}

SupportScreen::SupportScreen(rex::ReXApp* app, rex::ui::ImGuiDrawer* imgui_drawer,
                             rex::Runtime* runtime, ImFont* font, ImFont* title_font)
    : OverlayScreen(app, imgui_drawer, runtime, font, title_font) {
  GetContent(true);
}

void SupportScreen::OnDraw(ImGuiIO& io) {
  PollPad();
  if (closed()) {
    return;
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
      // A link is also written (not a PIX payload, too long to read).
      if (shown.url.rfind("http", 0) == 0) {
        ImGui::SetWindowFontScale(font_scale * 0.8f);
        ImVec2 url_size = ImGui::CalcTextSize(shown.url.c_str(), nullptr, false, qr_size);
        ImGui::SetCursorPos(ImVec2(qr_x + std::max(0.0f, (qr_size - url_size.x) / 2.0f),
                                   qr_y + qr_size + 24.0f * scale));
        ImGui::PushTextWrapPos(qr_x + qr_size);
        ImGui::TextColored(kDimText, "%s", shown.url.c_str());
        ImGui::PopTextWrapPos();
      }
    }

    ImGui::SetWindowFontScale(font_scale);
    DrawBackHint(draw, display, scale);
    if (font_) {
      ImGui::PopFont();
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

DonorsScreen::DonorsScreen(rex::ReXApp* app, rex::ui::ImGuiDrawer* imgui_drawer,
                           rex::Runtime* runtime, ImFont* font, ImFont* title_font)
    : OverlayScreen(app, imgui_drawer, runtime, font, title_font) {
  GetDonors(true);
}

void DonorsScreen::OnDraw(ImGuiIO& io) {
  uint16_t pressed = PollPad();
  if (closed()) {
    return;
  }
  const std::vector<Donor> list = GetDonors(false);
  const int count = int(list.size());
  if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) {
    selected_ = std::max(selected_ - 1, 0);
    scroll_to_ = selected_;
  }
  if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) {
    selected_ = std::max(std::min(selected_ + 1, count - 1), 0);
    scroll_to_ = selected_;
  }

  const ImVec2 display = io.DisplaySize;
  const float scale = display.y / 1080.0f;
  const float font_scale = scale * kFontScaleAt1080;
  const bool portuguese = IsPortuguese();

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##grfs_donors", nullptr, kFlags)) {
    ImDrawList* window_draw = ImGui::GetWindowDrawList();
    window_draw->AddRectFilledMultiColor(ImVec2(0, 0), display, kBackgroundTop, kBackgroundTop,
                                         kBackgroundBottom, kBackgroundBottom);
    if (font_) {
      ImGui::PushFont(font_);
    }
    const float left = display.x * 0.12f;
    const float width = display.x * 0.76f;

    // The title, a thank you and the donors' number.
    if (title_font_) {
      ImGui::PushFont(title_font_);
    }
    ImGui::SetWindowFontScale(font_scale * 0.9f);
    ImGui::SetCursorPos(ImVec2(left, 56.0f * scale));
    ImGui::TextColored(kText, "%s", portuguese ? "Doadores" : "Donors");
    if (title_font_) {
      ImGui::PopFont();
    }
    ImGui::SetWindowFontScale(font_scale);
    ImGui::SetCursorPosX(left);
    ImGui::TextColored(kDimText, "%s",
                       portuguese ? "Obrigado a todos que ajudam a manter o projeto ativo!"
                                  : "Thanks to everyone who helps keep the project active!");
    ImGui::Dummy(ImVec2(0, 20.0f * scale));

    // The list, scrolled to the selected donor.
    const float footer = 100.0f * scale;
    ImGui::SetCursorPosX(left);
    ImGui::BeginChild("##donors", ImVec2(width, display.y - ImGui::GetCursorPosY() - footer),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(font_scale);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float pad = 16.0f * scale, round = 10.0f * scale, edge = 3.0f * scale;
    if (!count) {
      ImGui::SetCursorPos(ImVec2(pad, pad));
      ImGui::TextColored(kDimText, "%s",
                         portuguese ? "Nenhum doador ainda. Seja o primeiro!"
                                    : "No donors yet. Be the first!");
    }
    for (int i = 0; i < count; ++i) {
      const Donor& donor = list[i];
      const bool focused = i == selected_;
      ImGui::PushID(i);
      ImGui::SetCursorPosX(edge);
      const ImVec2 top = ImGui::GetCursorScreenPos();
      const float row_width = width - edge * 2.0f;
      const float text_width = row_width - pad * 2.0f;
      const float name_height = ImGui::GetTextLineHeight();
      const float message_height =
          donor.message.empty()
              ? 0.0f
              : ImGui::CalcTextSize(donor.message.c_str(), nullptr, false, text_width).y;
      const float height = pad * 2.0f + name_height + (message_height ? message_height + 4.0f * scale : 0.0f);
      const ImVec2 bottom(top.x + row_width, top.y + height);
      draw->AddRectFilled(top, bottom,
                          focused ? IM_COL32(255, 255, 255, 38) : IM_COL32(255, 255, 255, 14),
                          round);
      if (focused) {
        draw->AddRect(ImVec2(top.x - edge, top.y - edge), ImVec2(bottom.x + edge, bottom.y + edge),
                      IM_COL32(255, 255, 255, 235), round + edge, 0, edge);
      }
      ImGui::SetCursorScreenPos(ImVec2(top.x + pad, top.y + pad));
      ImGui::TextColored(kText, "%s", donor.name.c_str());
      if (!donor.message.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(top.x + pad, top.y + pad + name_height + 4.0f * scale));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_width);
        ImGui::TextColored(kDimText, "%s", donor.message.c_str());
        ImGui::PopTextWrapPos();
      }
      ImGui::SetCursorScreenPos(ImVec2(top.x, bottom.y + 10.0f * scale));
      if (scroll_to_ == i) {
        ImGui::SetScrollFromPosY(top.y - ImGui::GetWindowPos().y, 0.4f);
        scroll_to_ = -1;
      }
      ImGui::Dummy(ImVec2(0, 0));
      ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SetWindowFontScale(font_scale);
    DrawBackHint(window_draw, display, scale);
    if (font_) {
      ImGui::PopFont();
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void NoticeScreen::OnDraw(ImGuiIO& io) {
  PollPad();
  if (closed()) {
    return;
  }
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
  if (ImGui::Begin("##grfs_notice", nullptr, kFlags)) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(ImVec2(0, 0), display, kBackgroundTop, kBackgroundTop,
                                  kBackgroundBottom, kBackgroundBottom);
    if (font_) {
      ImGui::PushFont(font_);
    }
    const float left = display.x * 0.20f, width = display.x * 0.60f;
    if (title_font_) {
      ImGui::PushFont(title_font_);
    }
    ImGui::SetWindowFontScale(font_scale * 0.9f);
    ImGui::SetCursorPos(ImVec2(left, display.y * 0.32f));
    ImGui::PushTextWrapPos(left + width);
    ImGui::TextColored(kText, "%s", title_.c_str());
    ImGui::PopTextWrapPos();
    if (title_font_) {
      ImGui::PopFont();
    }
    ImGui::SetWindowFontScale(font_scale);
    ImGui::SetCursorPos(ImVec2(left, ImGui::GetCursorPosY() + 30.0f * scale));
    ImGui::PushTextWrapPos(left + width);
    ImGui::TextColored(kDimText, "%s", text_.c_str());
    ImGui::PopTextWrapPos();
    DrawBackHint(draw, display, scale);
    if (font_) {
      ImGui::PopFont();
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}
