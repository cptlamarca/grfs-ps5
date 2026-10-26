// See support_screen.h. /support.txt (tools/news_server.py) has the title on
// its first line, the link on its second and the text after them;
// /donors.txt a donor per line: the name, a tab and a message.

#include "support_screen.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
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
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

#include "achievements_ui.h"
#include "grfs_app.h"
#include "iso_screen.h"
#include "news_hooks.h"
#include "screen_style.h"
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

// A heart, centred on c, s wide.
void DrawHeart(ImDrawList* draw, ImVec2 c, float s, ImU32 color) {
  const float r = s * 0.27f;
  draw->AddCircleFilled(ImVec2(c.x - r * 0.95f, c.y - r * 0.45f), r, color, 20);
  draw->AddCircleFilled(ImVec2(c.x + r * 0.95f, c.y - r * 0.45f), r, color, 20);
  draw->AddTriangleFilled(ImVec2(c.x - r * 1.9f, c.y - r * 0.2f), ImVec2(c.x + r * 1.9f, c.y - r * 0.2f),
                          ImVec2(c.x, c.y + s * 0.5f), color);
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

std::filesystem::path ProfileFile(const char* name) {
  return rex::system::kernel_state()->content_manager()->ResolveGameUserContentPath() / name;
}

constexpr char kDecisionFileName[] = "system_link_decision.txt";
constexpr char kChoicesFileName[] = "system_link_choices.txt";

// The texts of the game's language file (ID:type:mask:text lines) by ID.
std::map<uint32_t, std::string> GameTexts(const std::filesystem::path& game_data_root) {
  std::map<uint32_t, std::string> texts;
  std::ifstream file(game_data_root / "Language" /
                     (IsPortuguese() ? "Oasis_Brazil.inf" : "Oasis_English.inf"));
  for (std::string line; std::getline(file, line);) {
    size_t a = line.find(':'), b = a == std::string::npos ? a : line.find(':', a + 1);
    size_t c = b == std::string::npos ? b : line.find(':', b + 1);
    if (c == std::string::npos || line.empty() || !std::isdigit(uint8_t(line[0]))) {
      continue;
    }
    std::string text = line.substr(c + 1);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    texts.emplace(uint32_t(std::stoul(line.substr(0, a))), text);
  }
  return texts;
}

// A plain line drawing of an item's kind, by its type and number in the
// game's unlockables (1 a weapon, 2 a grenade, 3 an attachment: launchers,
// ammunition or optics, 5 a gadget), centred on c, s wide.
void DrawItemIcon(ImDrawList* draw, ImVec2 c, float s, uint32_t type, uint32_t number,
                  ImU32 color) {
  const float t = s * 0.035f;
  auto p = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };
  auto box = [&](float x0, float y0, float x1, float y1) {
    draw->AddRectFilled(p(x0, y0), p(x1, y1), color, s * 0.02f);
  };
  auto ring = [&](float x, float y, float r) { draw->AddCircle(p(x, y), r * s, color, 32, t); };
  if (type == 1) {  // a rifle
    box(-0.48f, -0.06f, -0.30f, 0.08f);   // stock
    box(-0.30f, -0.08f, 0.18f, 0.04f);    // body
    box(0.18f, -0.05f, 0.48f, -0.01f);    // barrel
    box(-0.06f, 0.04f, 0.04f, 0.24f);     // magazine
    box(-0.22f, 0.04f, -0.14f, 0.18f);    // grip
    box(-0.16f, -0.16f, 0.06f, -0.10f);   // sight
  } else if (type == 2) {  // a grenade
    draw->AddCircleFilled(p(0.0f, 0.08f), 0.24f * s, color, 32);
    box(-0.08f, -0.24f, 0.08f, -0.14f);
    draw->AddLine(p(0.08f, -0.22f), p(0.26f, -0.04f), color, t * 1.5f);
    ring(-0.16f, -0.24f, 0.06f);
  } else if (type == 3 && (number == 2 || number == 3 || number == 4 || number == 6)) {
    // an under-barrel launcher and its round
    box(-0.44f, -0.12f, 0.20f, 0.06f);
    box(-0.30f, 0.06f, -0.20f, 0.22f);
    box(0.24f, -0.10f, 0.32f, 0.04f);
    draw->AddCircleFilled(p(0.40f, -0.03f), 0.07f * s, color, 24);
  } else if (type == 3 && number >= 37 && number <= 41) {  // ammunition
    for (float x : {-0.24f, 0.0f, 0.24f}) {
      box(x - 0.07f, -0.10f, x + 0.07f, 0.28f);
      draw->AddTriangleFilled(p(x - 0.07f, -0.10f), p(x + 0.07f, -0.10f), p(x, -0.30f), color);
    }
  } else if (type == 3) {  // an optic: its tube, lenses and a reticle
    box(-0.30f, -0.07f, 0.30f, 0.07f);
    box(-0.44f, -0.13f, -0.30f, 0.13f);
    box(0.30f, -0.11f, 0.44f, 0.11f);
    box(-0.10f, 0.07f, -0.04f, 0.18f);
    box(0.04f, 0.07f, 0.10f, 0.18f);
    ring(0.0f, -0.30f, 0.12f);
    draw->AddLine(p(-0.16f, -0.30f), p(0.16f, -0.30f), color, t);
    draw->AddLine(p(0.0f, -0.46f), p(0.0f, -0.14f), color, t);
  } else if (type == 5 && (number == 7 || number == 12 || number == 15)) {
    // a drone: its body and four rotors
    box(-0.12f, -0.08f, 0.12f, 0.08f);
    for (float x : {-0.30f, 0.30f}) {
      for (float y : {-0.22f, 0.22f}) {
        draw->AddLine(p(x * 0.4f, y * 0.4f), p(x, y), color, t * 1.5f);
        ring(x, y, 0.12f);
      }
    }
  } else {  // another gadget: a case with a light
    box(-0.34f, -0.18f, 0.34f, 0.24f);
    box(-0.12f, -0.28f, 0.12f, -0.18f);
    draw->AddCircleFilled(p(0.0f, 0.03f), 0.08f * s, IM_COL32(20, 32, 52, 255), 24);
  }
}

}  // namespace

float OverlayScreen::ShownSeconds() {
  const double now = ImGui::GetTime();
  if (shown_at_ < 0.0) {
    shown_at_ = now;
  }
  return float(now - shown_at_);
}

void OverlayScreen::DrawBackground(ImDrawList* draw, ImVec2 display, float t, float scale,
                                   SkullShade shade) {
  float aspect = 1.6f;
  rex::ui::ImmediateTexture* texture =
      GrfsApp::instance() ? GrfsApp::instance()->SkullTexture(aspect) : nullptr;
  DrawSkullBackground(draw, texture, aspect, display, t, scale, shade);
}

void OverlayScreen::FadeIn(ImVec2 display, float appear) {
  if (appear < 1.0f) {
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), display,
                                                  IM_COL32(0, 0, 0, int(200 * (1.0f - appear))));
  }
}

uint16_t OverlayScreen::PollPad() {
  // Nothing until the button that opened the screen is up; Circle or Back
  // closes it, hidden once that one is up too, so the game doesn't get it.
  auto* input = static_cast<rex::input::InputSystem*>(runtime_->input_system());
  rex::input::X_INPUT_STATE state = {};
  if (!input || input->GetRawState(0, &state) != 0 /* X_ERROR_SUCCESS */) {
    state = {};
  }
  uint16_t buttons = state.gamepad.buttons;
  if (state.gamepad.thumb_lx > 16000) {
    buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
  } else if (state.gamepad.thumb_lx < -16000) {
    buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
  }
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
  const float t = ShownSeconds();
  const float appear = EaseOut(std::clamp(t / 0.7f, 0.0f, 1.0f));
  const float body = EaseOut(std::clamp((t - 0.2f) / 0.6f, 0.0f, 1.0f));
  const float card = EaseOut(std::clamp((t - 0.35f) / 0.6f, 0.0f, 1.0f));
  const float pulse = 0.5f + 0.5f * std::sin(t * 2.4f);

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##grfs_support", nullptr, kFlags)) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    DrawBackground(draw, display, t, scale, SkullShade::kLeft);
    if (font_) {
      ImGui::PushFont(font_);
    }
    const float left = display.x * 0.08f;
    const float qr_size = display.y * 0.44f;
    const float qr_x = display.x * 0.90f - qr_size;
    const float text_right = qr_x - 110.0f * scale;
    const float top = display.y * 0.14f;
    const float slide = (1.0f - appear) * -70.0f * scale;
    DrawHudCorners(draw, ImVec2(left - 40.0f * scale, top - 50.0f * scale),
                   ImVec2(text_right + 30.0f * scale, display.y - 70.0f * scale), scale, appear);

    // The kicker, the title (sliding in) and the text.
    ImGui::SetWindowFontScale(font_scale * 0.6f);
    ImGui::SetCursorPos(ImVec2(left + slide, top));
    ImGui::TextColored(WithAlpha(kAccent, appear), "%s", "GHOST RECON: FUTURE SOLDIER  //  PS5");
    if (title_font_) {
      ImGui::PushFont(title_font_);
    }
    ImGui::SetWindowFontScale(font_scale * 0.9f);
    ImGui::SetCursorPos(ImVec2(left + slide * 1.6f, ImGui::GetCursorPosY() + 4.0f * scale));
    ImGui::PushTextWrapPos(text_right);
    ImGui::TextColored(WithAlpha(kText, appear), "%s", shown.title.c_str());
    ImGui::PopTextWrapPos();
    if (title_font_) {
      ImGui::PopFont();
    }
    {
      const float y = ImGui::GetCursorPosY() + 6.0f * scale;
      draw->AddRectFilled(ImVec2(left, y), ImVec2(left + 140.0f * scale * appear, y + 4.0f * scale),
                          IM_COL32(90, 200, 255, 255));
      ImGui::SetCursorPosY(y + 34.0f * scale);
    }
    ImGui::SetWindowFontScale(font_scale * 0.8f);
    ImGui::SetCursorPosX(left);
    ImGui::PushTextWrapPos(text_right);
    ImGui::TextColored(WithAlpha(kDimText, body), "%s", shown.text.c_str());
    ImGui::PopTextWrapPos();

    // The QR code on a card, right, in a pulsing frame.
    if (!shown.url.empty()) {
      const float qr_y = (display.y - qr_size) / 2 - 20.0f * scale + (1.0f - card) * 40.0f * scale;
      const float frame = 18.0f * scale;
      for (int ring = 3; ring >= 1; --ring) {
        const float grow = frame + ring * 10.0f * scale * (0.8f + 0.4f * pulse);
        draw->AddRectFilled(ImVec2(qr_x - grow, qr_y - grow),
                            ImVec2(qr_x + qr_size + grow, qr_y + qr_size + grow),
                            IM_COL32(70, 190, 255, int((8 + 6 * pulse) * card)), 22.0f * scale);
      }
      draw->AddRectFilled(ImVec2(qr_x - frame, qr_y - frame),
                          ImVec2(qr_x + qr_size + frame, qr_y + qr_size + frame),
                          IM_COL32(6, 14, 20, int(220 * card)), 18.0f * scale);
      draw->AddRect(ImVec2(qr_x - frame, qr_y - frame),
                    ImVec2(qr_x + qr_size + frame, qr_y + qr_size + frame),
                    IM_COL32(110, 210, 255, int((120 + 120 * pulse) * card)), 18.0f * scale, 0,
                    3.0f * scale);
      try {
        qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(shown.url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        // A white card with a quiet zone of 3 modules.
        const int size = qr.getSize();
        const float module = qr_size / float(size + 6);
        draw->AddRectFilled(ImVec2(qr_x, qr_y), ImVec2(qr_x + qr_size, qr_y + qr_size),
                            IM_COL32(255, 255, 255, int(255 * card)), 12.0f * scale);
        for (int y = 0; y < size; ++y) {
          for (int x = 0; x < size; ++x) {
            if (qr.getModule(x, y)) {
              ImVec2 corner(qr_x + (x + 3) * module, qr_y + (y + 3) * module);
              draw->AddRectFilled(corner, ImVec2(corner.x + module + 0.5f, corner.y + module + 0.5f),
                                  IM_COL32(0, 0, 0, int(255 * card)));
            }
          }
        }
      } catch (const std::exception&) {
        // (a link too long for a QR code: only its text)
      }
      // What to do with it: a link is written, a PIX payload (too long to
      // read) is to be scanned.
      const bool link = shown.url.rfind("http", 0) == 0;
      const std::string caption =
          link ? shown.url
               : (IsPortuguese() ? "Escaneie com o app do seu banco" : "Scan it with your bank's app");
      ImGui::SetWindowFontScale(font_scale * 0.75f);
      const ImVec2 caption_size = ImGui::CalcTextSize(caption.c_str(), nullptr, false, qr_size);
      ImGui::SetCursorPos(ImVec2(qr_x + std::max(0.0f, (qr_size - caption_size.x) / 2.0f),
                                 qr_y + qr_size + frame + 22.0f * scale));
      ImGui::PushTextWrapPos(qr_x + qr_size);
      ImGui::TextColored(WithAlpha(link ? kAccent : kDimText, card), "%s", caption.c_str());
      ImGui::PopTextWrapPos();
    }

    ImGui::SetWindowFontScale(font_scale);
    DrawBackHint(draw, display, scale);
    if (font_) {
      ImGui::PopFont();
    }
    FadeIn(display, appear);
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
  }
  if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) {
    selected_ = std::max(std::min(selected_ + 1, count - 1), 0);
  }

  const ImVec2 display = io.DisplaySize;
  const float scale = display.y / 1080.0f;
  const float font_scale = scale * kFontScaleAt1080;
  const bool portuguese = IsPortuguese();
  const float t = ShownSeconds();
  const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
  const float appear = EaseOut(std::clamp(t / 0.7f, 0.0f, 1.0f));
  const float pulse = 0.5f + 0.5f * std::sin(t * 3.2f);

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##grfs_donors", nullptr, kFlags)) {
    ImDrawList* window_draw = ImGui::GetWindowDrawList();
    DrawBackground(window_draw, display, t, scale, SkullShade::kAll);
    if (font_) {
      ImGui::PushFont(font_);
    }
    const float left = display.x * 0.12f;
    const float width = display.x * 0.76f;
    const float top = 56.0f * scale;
    const float slide = (1.0f - appear) * -60.0f * scale;

    // The kicker, the title, a thank you; the donors' number, right.
    ImGui::SetWindowFontScale(font_scale * 0.6f);
    ImGui::SetCursorPos(ImVec2(left + slide * 0.5f, top));
    ImGui::TextColored(WithAlpha(kAccent, appear), "%s",
                       portuguese ? "QUEM MANTÉM O PROJETO VIVO" : "WHO KEEPS THE PROJECT ALIVE");
    if (title_font_) {
      ImGui::PushFont(title_font_);
    }
    ImGui::SetWindowFontScale(font_scale * 0.9f);
    ImGui::SetCursorPos(ImVec2(left + slide, ImGui::GetCursorPosY() + 4.0f * scale));
    ImGui::TextColored(WithAlpha(kText, appear), "%s", portuguese ? "Doadores" : "Donors");
    if (title_font_) {
      ImGui::PopFont();
    }
    ImGui::SetWindowFontScale(font_scale * 0.8f);
    ImGui::SetCursorPosX(left + slide * 0.5f);
    ImGui::TextColored(WithAlpha(kDimText, appear), "%s",
                       portuguese ? "Obrigado a todos que ajudam a manter o projeto ativo!"
                                  : "Thanks to everyone who helps keep the project active!");
    if (count) {
      ImGui::SetWindowFontScale(font_scale * 0.8f);
      const std::string badge = std::to_string(count) +
                                (portuguese ? (count == 1 ? " doador" : " doadores")
                                            : (count == 1 ? " donor" : " donors"));
      const float badge_h = 52.0f * scale;
      const float badge_w = ImGui::CalcTextSize(badge.c_str()).x + 92.0f * scale;
      const ImVec2 a(left + width - badge_w, top + 30.0f * scale);
      window_draw->AddRectFilled(a, ImVec2(a.x + badge_w, a.y + badge_h),
                                 IM_COL32(255, 255, 255, int(16 * appear)), badge_h / 2);
      window_draw->AddRect(a, ImVec2(a.x + badge_w, a.y + badge_h),
                           IM_COL32(240, 100, 130, int(120 * appear)), badge_h / 2, 0, 2.0f * scale);
      DrawHeart(window_draw, ImVec2(a.x + 34.0f * scale, a.y + badge_h / 2), 26.0f * scale,
                IM_COL32(240, 100, 130, int(255 * appear)));
      window_draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                           ImVec2(a.x + 62.0f * scale, a.y + (badge_h - ImGui::GetFontSize()) / 2),
                           IM_COL32(255, 255, 255, int(255 * appear)), badge.c_str());
    }
    ImGui::Dummy(ImVec2(0, 26.0f * scale));

    // The list, easing to the selected donor.
    const float footer = 100.0f * scale;
    const float list_top = ImGui::GetCursorPosY();
    const float list_height = display.y - list_top - footer;
    ImGui::SetCursorPos(ImVec2(left, list_top));
    ImGui::BeginChild("##donors", ImVec2(width, list_height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(font_scale);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float pad = 20.0f * scale, round = 10.0f * scale, edge = 3.0f * scale;
    const float heart = 34.0f * scale;
    if (!count) {
      ImGui::SetCursorPos(ImVec2(pad, pad));
      ImGui::TextColored(WithAlpha(kDimText, appear * (0.65f + 0.35f * pulse)), "%s",
                         portuguese ? "Nenhum doador ainda. Seja o primeiro!"
                                    : "No donors yet. Be the first!");
    }
    float focus_top = 0.0f, focus_height = 0.0f;
    for (int i = 0; i < count; ++i) {
      const Donor& donor = list[i];
      const bool focused = i == selected_;
      const float row_alpha =
          EaseOut(std::clamp((t - 0.2f - 0.06f * std::min(i, 10)) / 0.4f, 0.0f, 1.0f));
      const float shift = (1.0f - row_alpha) * 40.0f * scale;
      ImGui::PushID(i);
      ImGui::SetCursorPosX(edge);
      const float row_y = ImGui::GetCursorPosY();
      const ImVec2 top_left = ImGui::GetCursorScreenPos();
      const float row_width = width - edge * 2.0f;
      const float text_x = top_left.x + pad + heart + 24.0f * scale + shift;
      const float text_width = top_left.x + row_width - pad - text_x;
      const float name_height = ImGui::GetTextLineHeight();
      ImGui::SetWindowFontScale(font_scale * 0.85f);
      const float message_height =
          donor.message.empty()
              ? 0.0f
              : ImGui::CalcTextSize(donor.message.c_str(), nullptr, false, text_width).y;
      ImGui::SetWindowFontScale(font_scale);
      const float height =
          pad * 2.0f + name_height + (message_height ? message_height + 6.0f * scale : 0.0f);
      const ImVec2 bottom(top_left.x + row_width, top_left.y + height);
      if (focused) {
        focus_top = row_y;
        focus_height = height;
      }
      draw->AddRectFilled(top_left, bottom, IM_COL32(8, 14, 20, int(185 * row_alpha)), round);
      draw->AddRectFilledMultiColor(top_left, ImVec2(top_left.x + row_width * 0.5f, bottom.y),
                                    IM_COL32(240, 100, 130, int(30 * row_alpha)),
                                    IM_COL32(240, 100, 130, 0), IM_COL32(240, 100, 130, 0),
                                    IM_COL32(240, 100, 130, int(30 * row_alpha)));
      draw->AddRectFilled(top_left, ImVec2(top_left.x + 6.0f * scale, bottom.y),
                          IM_COL32(240, 100, 130, int(230 * row_alpha)), round,
                          ImDrawFlags_RoundCornersLeft);
      if (focused) {
        draw->AddRectFilled(top_left, bottom, IM_COL32(255, 255, 255, 16), round);
        draw->AddRect(ImVec2(top_left.x - edge * 0.5f, top_left.y - edge * 0.5f),
                      ImVec2(bottom.x + edge * 0.5f, bottom.y + edge * 0.5f),
                      IM_COL32(110, 210, 255, int(150 + 105 * pulse)), round + edge, 0, edge);
      }
      // The heart, beating on the selected donor.
      const float beat = focused ? 1.0f + 0.12f * std::max(0.0f, std::sin(t * 6.0f)) : 1.0f;
      DrawHeart(draw, ImVec2(top_left.x + pad + 8.0f * scale + heart / 2 + shift,
                             top_left.y + pad + name_height / 2),
                heart * beat, IM_COL32(240, 100, 130, int(255 * row_alpha)));
      ImGui::SetCursorScreenPos(ImVec2(text_x, top_left.y + pad));
      ImGui::TextColored(WithAlpha(kText, row_alpha), "%s", donor.name.c_str());
      if (!donor.message.empty()) {
        ImGui::SetWindowFontScale(font_scale * 0.85f);
        ImGui::SetCursorScreenPos(ImVec2(text_x, top_left.y + pad + name_height + 6.0f * scale));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_width);
        ImGui::TextColored(WithAlpha(kDimText, row_alpha), "%s", donor.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SetWindowFontScale(font_scale);
      }
      ImGui::SetCursorScreenPos(ImVec2(top_left.x, bottom.y + 10.0f * scale));
      ImGui::Dummy(ImVec2(0, 0));
      ImGui::PopID();
    }
    {
      const float max_scroll = ImGui::GetScrollMaxY();
      const float target = std::clamp(focus_top + focus_height / 2 - list_height / 2, 0.0f,
                                      std::max(max_scroll, 0.0f));
      scroll_ += (target - scroll_) * std::min(1.0f, dt * 12.0f);
      if (std::abs(target - scroll_) < 0.5f) {
        scroll_ = target;
      }
      ImGui::SetScrollY(scroll_);
    }
    ImGui::EndChild();
    // The list fades out at its top and bottom edges.
    {
      const ImU32 opaque = IM_COL32(3, 8, 12, 200), clear = IM_COL32(3, 8, 12, 0);
      const float fade = 28.0f * scale;
      window_draw->AddRectFilledMultiColor(ImVec2(left, list_top), ImVec2(left + width, list_top + fade),
                                           opaque, opaque, clear, clear);
      window_draw->AddRectFilledMultiColor(ImVec2(left, list_top + list_height - fade),
                                           ImVec2(left + width, list_top + list_height), clear,
                                           clear, opaque, opaque);
    }

    ImGui::SetWindowFontScale(font_scale);
    DrawBackHint(window_draw, display, scale);
    if (font_) {
      ImGui::PopFont();
    }
    FadeIn(display, appear);
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

DecisionScreen::DecisionScreen(rex::ReXApp* app, rex::ui::ImGuiDrawer* imgui_drawer,
                               rex::Runtime* runtime, ImFont* font, ImFont* title_font,
                               const std::filesystem::path& game_data_root)
    : OverlayScreen(app, imgui_drawer, runtime, font, title_font) {
  // ID, class, rank, then each choice's titles' text IDs.
  std::ifstream file(ProfileFile(kDecisionFileName));
  std::string line;
  std::getline(file, line);
  std::istringstream fields(line);
  std::string choices[2];
  fields >> id_ >> row_ >> rank_;
  fields.ignore(1);
  std::getline(fields, choices[0], '\t');
  std::getline(fields, choices[1], '\t');
  uint32_t descriptions[2] = {};
  fields >> descriptions[0] >> descriptions[1];
  for (int i = 0; i < 2; ++i) {
    char colon = 0;
    fields >> types_[i] >> colon >> numbers_[i];
  }
  const std::map<uint32_t, std::string> texts = GameTexts(game_data_root);
  for (int i = 0; i < 2; ++i) {
    auto text = texts.find(descriptions[i]);
    if (descriptions[i] && text != texts.end()) {
      texts_[i] = text->second;
    }
  }
  for (int i = 0; i < 2; ++i) {
    std::istringstream ids(choices[i]);
    for (std::string id; std::getline(ids, id, ',');) {
      auto text = id.empty() ? texts.end() : texts.find(uint32_t(std::stoul(id)));
      if (text != texts.end()) {
        choices_[i] += (choices_[i].empty() ? "" : " / ") + text->second;
      }
    }
  }
}

std::filesystem::path DecisionScreen::Path() {
  return ProfileFile(kDecisionFileName);
}

bool DecisionScreen::Waiting() {
  std::error_code error;
  return std::filesystem::exists(ProfileFile(kDecisionFileName), error);
}

void DecisionScreen::OnDraw(ImGuiIO& io) {
  uint16_t pressed = PollPad();
  if (closed()) {
    return;
  }
  if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) {
    selected_ = 0;
  }
  if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) {
    selected_ = 1;
  }
  if ((pressed & rex::input::X_INPUT_GAMEPAD_A) && id_) {
    // The choice is kept; the items come into the class when the game works
    // out its progression again (the characters menu, a match).
    std::ofstream(ProfileFile(kChoicesFileName), std::ios::app) << id_ << ' ' << selected_ + 1
                                                                << '\n';
    std::error_code error;
    std::filesystem::remove(ProfileFile(kDecisionFileName), error);
    Close();
  }

  const ImVec2 display = io.DisplaySize;
  const float scale = display.y / 1080.0f;
  const float font_scale = scale * kFontScaleAt1080;
  const bool portuguese = IsPortuguese();
  static const char* const kClassesPt[] = {"Batedor", "Engenheiro", "Atirador"};
  static const char* const kClassesEn[] = {"Scout", "Engineer", "Rifleman"};
  const char* class_name = row_ < 3 ? (portuguese ? kClassesPt : kClassesEn)[row_] : "";

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##grfs_decision", nullptr, kFlags)) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(ImVec2(0, 0), display, kBackgroundTop, kBackgroundTop,
                                  kBackgroundBottom, kBackgroundBottom);
    if (font_) {
      ImGui::PushFont(font_);
    }
    const float left = display.x * 0.12f, width = display.x * 0.76f;
    if (title_font_) {
      ImGui::PushFont(title_font_);
    }
    ImGui::SetWindowFontScale(font_scale * 0.9f);
    ImGui::SetCursorPos(ImVec2(left, display.y * 0.18f));
    ImGui::TextColored(kText, "%s", portuguese ? "Ponto de Decisão" : "Decision Point");
    if (title_font_) {
      ImGui::PopFont();
    }
    ImGui::SetWindowFontScale(font_scale);
    ImGui::SetCursorPosX(left);
    ImGui::TextColored(kDimText,
                       portuguese ? "%s chegou ao nível %u. Escolha um dos itens para liberar:"
                                  : "%s reached level %u. Choose one of the items to unlock:",
                       class_name, rank_);

    // The two choices side by side, the selected one lit.
    const float gap = 40.0f * scale, box_width = (width - gap) / 2.0f;
    const float box_top = display.y * 0.30f, box_height = display.y * 0.44f;
    const float round = 12.0f * scale, edge = 4.0f * scale;
    for (int i = 0; i < 2; ++i) {
      const ImVec2 top(left + i * (box_width + gap), box_top);
      const ImVec2 bottom(top.x + box_width, top.y + box_height);
      const bool focused = i == selected_;
      draw->AddRectFilled(top, bottom,
                          focused ? IM_COL32(40, 80, 130, 230) : IM_COL32(20, 32, 52, 200),
                          round);
      if (focused) {
        draw->AddRect(top, bottom, IM_COL32(150, 210, 255, 255), round, 0, edge);
      }
      // The item's icon, its name and its description.
      const float pad = 30.0f * scale, icon = box_height * 0.30f;
      DrawItemIcon(draw, ImVec2(top.x + box_width / 2.0f, top.y + pad + icon / 2.0f), icon * 1.6f,
                   types_[i], numbers_[i],
                   focused ? IM_COL32(220, 240, 255, 255) : IM_COL32(150, 165, 185, 255));
      const std::string& name = choices_[i].empty() ? std::string("?") : choices_[i];
      ImGui::SetWindowFontScale(font_scale * 1.3f);
      const ImVec2 size = ImGui::CalcTextSize(name.c_str(), nullptr, false, box_width - pad * 2);
      ImGui::SetCursorPos(ImVec2(top.x + (box_width - size.x) / 2.0f, top.y + pad * 1.5f + icon));
      ImGui::PushTextWrapPos(bottom.x - pad);
      ImGui::TextColored(focused ? kText : kDimText, "%s", name.c_str());
      ImGui::PopTextWrapPos();
      ImGui::SetWindowFontScale(font_scale * 0.85f);
      ImGui::SetCursorPos(ImVec2(top.x + pad, ImGui::GetCursorPosY() + 16.0f * scale));
      ImGui::PushTextWrapPos(bottom.x - pad);
      ImGui::TextColored(kDimText, "%s", texts_[i].c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::SetWindowFontScale(font_scale);
    ImGui::SetCursorPos(ImVec2(left, box_top + box_height + 30.0f * scale));
    ImGui::PushTextWrapPos(left + width);
    ImGui::TextColored(kDimText, "%s",
                       portuguese ? "A escolha é definitiva. Pressione X para confirmar ou O "
                                    "para decidir depois (a escolha volta a aparecer no lobby)."
                                  : "The choice is final. Press X to confirm, or O to decide "
                                    "later (it comes back in the lobby).");
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
