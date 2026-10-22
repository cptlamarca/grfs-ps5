// Trophies screen and notifications in the style of the PS5's, for the game's
// Xbox 360 achievements (graded by their gamerscore).

#include "achievements_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/flags.h>

#include "grfs_app.h"
#include "trophy_sound.h"

namespace {

// The fonts are rasterized for 2160p and scaled down to the display.
constexpr float kFontScaleAt1080 = 0.5f;

struct Texts {
  const char* title;
  const char* game;
  const char* earned_format;  // earned, total
  const char* grades[4];      // bronze, silver, gold, platinum
  const char* platinum_description;
  const char* back;
  const char* scroll;
  const char* notification;
  const char* date_format;
};

const Texts& GetTexts() {
  static const Texts kEnglish = {"Trophies",
                                 "Tom Clancy's Ghost Recon: Future Soldier",
                                 "%d/%d trophies",
                                 {"Bronze", "Silver", "Gold", "Platinum"},
                                 "Earn all the other trophies",
                                 "Back",
                                 "Scroll",
                                 "You've earned a trophy.",
                                 "%m/%d/%Y"};
  static const Texts kPortuguese = {"Troféus",
                                    "Tom Clancy's Ghost Recon: Future Soldier",
                                    "%d/%d troféus",
                                    {"Bronze", "Prata", "Ouro", "Platina"},
                                    "Ganhe todos os outros troféus",
                                    "Voltar",
                                    "Rolar",
                                    "Você ganhou um troféu.",
                                    "%d/%m/%Y"};
  return IsPortuguese() ? kPortuguese : kEnglish;
}

enum Grade { kBronze, kSilver, kGold, kPlatinum };

Grade GradeOf(uint32_t gamerscore) {
  return gamerscore <= 10 ? kBronze : gamerscore <= 25 ? kSilver : kGold;
}

// Points of the PS5's trophy levels, which its percentages are made of.
constexpr int kGradePoints[] = {15, 30, 90, 300};

constexpr ImU32 kGradeColors[] = {IM_COL32(205, 127, 72, 255), IM_COL32(196, 204, 214, 255),
                                  IM_COL32(236, 196, 82, 255), IM_COL32(172, 206, 240, 255)};
constexpr ImU32 kGradeShades[] = {IM_COL32(128, 72, 38, 255), IM_COL32(120, 128, 140, 255),
                                  IM_COL32(170, 124, 30, 255), IM_COL32(98, 128, 170, 255)};

constexpr ImU32 kBackgroundTop = IM_COL32(16, 30, 54, 255);
constexpr ImU32 kBackgroundBottom = IM_COL32(4, 8, 16, 255);
constexpr ImU32 kRow = IM_COL32(255, 255, 255, 14);
constexpr ImU32 kRowFocused = IM_COL32(255, 255, 255, 38);
constexpr ImU32 kFocusEdge = IM_COL32(255, 255, 255, 235);
constexpr ImU32 kBarBack = IM_COL32(255, 255, 255, 40);
constexpr ImU32 kBarFill = IM_COL32(255, 255, 255, 230);
constexpr ImU32 kCard = IM_COL32(32, 34, 40, 242);
constexpr ImVec4 kText = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
constexpr ImVec4 kDimText = ImVec4(0.70f, 0.73f, 0.78f, 1.0f);
constexpr ImVec4 kLockedText = ImVec4(0.55f, 0.58f, 0.63f, 1.0f);

// A trophy cup: a bowl with handles on a stem and a base.
void DrawTrophy(ImDrawList* draw, ImVec2 center, float size, Grade grade, bool earned = true) {
  ImU32 color = kGradeColors[grade], shade = kGradeShades[grade];
  if (!earned) {
    color = IM_COL32(90, 96, 106, 255);
    shade = IM_COL32(60, 64, 72, 255);
  }
  const float s = size / 2.0f;
  const float top = center.y - s * 0.75f;
  // Handles.
  draw->AddCircle(ImVec2(center.x - s * 0.62f, top + s * 0.30f), s * 0.24f, shade, 16,
                  s * 0.12f);
  draw->AddCircle(ImVec2(center.x + s * 0.62f, top + s * 0.30f), s * 0.24f, shade, 16,
                  s * 0.12f);
  // Bowl: a half disc under its rim.
  draw->PathArcTo(ImVec2(center.x, top), s * 0.62f, 0.0f, 3.14159265f, 20);
  draw->PathFillConvex(color);
  draw->AddRectFilled(ImVec2(center.x - s * 0.66f, top - s * 0.10f),
                      ImVec2(center.x + s * 0.66f, top + s * 0.04f), shade);
  // Stem and base.
  draw->AddRectFilled(ImVec2(center.x - s * 0.10f, top + s * 0.55f),
                      ImVec2(center.x + s * 0.10f, center.y + s * 0.55f), color);
  draw->AddRectFilled(ImVec2(center.x - s * 0.42f, center.y + s * 0.55f),
                      ImVec2(center.x + s * 0.42f, center.y + s * 0.80f), shade, s * 0.06f);
}

// The Circle and d-pad glyphs of the button hints.
void DrawCircleButton(ImDrawList* draw, ImVec2 center, float radius) {
  draw->AddCircleFilled(center, radius, IM_COL32(255, 255, 255, 235), 24);
  draw->AddCircle(center, radius * 0.5f, IM_COL32(20, 24, 32, 255), 24, radius * 0.17f);
}

void DrawDpad(ImDrawList* draw, ImVec2 center, float radius) {
  const float w = radius * 0.36f;
  draw->AddRectFilled(ImVec2(center.x - w, center.y - radius),
                      ImVec2(center.x + w, center.y + radius), IM_COL32(255, 255, 255, 235), 2.0f);
  draw->AddRectFilled(ImVec2(center.x - radius, center.y - w),
                      ImVec2(center.x + radius, center.y + w), IM_COL32(255, 255, 255, 235), 2.0f);
}

std::string FormatDate(uint64_t filetime, const char* format) {
  constexpr uint64_t kUnixEpoch = 116444736000000000ull;
  if (filetime <= kUnixEpoch) {
    return {};
  }
  std::time_t time = std::time_t((filetime - kUnixEpoch) / 10000000ull);
  std::tm local = {};
#ifdef _WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  char text[32];
  std::strftime(text, sizeof(text), format, &local);
  return text;
}

}  // namespace

bool IsPortuguese() {
  // XLanguage 9 is Portuguese.
  return REXCVAR_GET(user_language) == 9;
}

AchievementsScreen::AchievementsScreen(rex::ReXApp* app, rex::ui::ImGuiDrawer* imgui_drawer,
                                       rex::ui::ImmediateDrawer* immediate_drawer,
                                       rex::Runtime* runtime,
                                       rex::system::AchievementManager* achievements,
                                       ImFont* font, ImFont* title_font)
    : ImGuiDialog(imgui_drawer),
      app_(app),
      runtime_(runtime),
      achievements_(achievements),
      icon_cache_(immediate_drawer, runtime),
      font_(font),
      title_font_(title_font) {}

uint16_t AchievementsScreen::PollPad() {
  auto* input = static_cast<rex::input::InputSystem*>(runtime_->input_system());
  rex::input::X_INPUT_STATE state = {};
  if (!input || input->GetRawState(0, &state) != 0 /* X_ERROR_SUCCESS */) {
    state = {};
  }
  uint16_t buttons = state.gamepad.buttons;
  // The left stick moves like the d-pad.
  int16_t ly = state.gamepad.thumb_ly;
  if (ly > 16000) {
    buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_UP;
  } else if (ly < -16000) {
    buttons |= rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
  }
  if (!pad_released_) {
    pad_released_ = !buttons;
    previous_buttons_ = buttons;
    return 0;
  }
  uint16_t pressed = buttons & ~previous_buttons_;
  // Held up or down repeats, after a pause.
  constexpr uint16_t kVertical =
      rex::input::X_INPUT_GAMEPAD_DPAD_UP | rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
  double now = ImGui::GetTime();
  if (pressed & kVertical) {
    repeat_time_ = now + 0.35;
  } else if ((buttons & kVertical) && now >= repeat_time_) {
    pressed |= buttons & kVertical;
    repeat_time_ = now + 0.08;
  }
  previous_buttons_ = buttons;
  return closing_ && !buttons ? uint16_t(0xFFFF) : pressed;
}

void AchievementsScreen::OnDraw(ImGuiIO& io) {
  const Texts& texts = GetTexts();
  const auto list = achievements_->ListAchievements();
  // The platinum trophy comes first, then the achievements.
  const int count = int(list.size()) + 1;

  uint16_t pressed = PollPad();
  if (pressed == 0xFFFF) {
    // Hidden once the button that closed it is up, so the game doesn't get it.
    app_->SetAchievementsOverlayVisible(false);
    return;
  }
  if (pressed & (rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_BACK)) {
    closing_ = true;
  }
  if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) {
    selected_ = std::max(selected_ - 1, 0);
    scroll_to_ = selected_;
  }
  if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) {
    selected_ = std::min(selected_ + 1, count - 1);
    scroll_to_ = selected_;
  }

  int earned_count[4] = {}, total_count[4] = {};
  int earned_points = 0, total_points = 0;
  for (const auto& achievement : list) {
    Grade grade = GradeOf(achievement.gamerscore);
    ++total_count[grade];
    total_points += kGradePoints[grade];
    if (achievements_->IsUnlocked(achievement.id)) {
      ++earned_count[grade];
      earned_points += kGradePoints[grade];
    }
  }
  const bool platinum =
      !list.empty() &&
      earned_count[kBronze] + earned_count[kSilver] + earned_count[kGold] == int(list.size());
  total_count[kPlatinum] = 1;
  earned_count[kPlatinum] = platinum;
  total_points += kGradePoints[kPlatinum];
  earned_points += platinum ? kGradePoints[kPlatinum] : 0;
  const int percent = total_points ? earned_points * 100 / total_points : 0;
  const int earned_total =
      earned_count[kBronze] + earned_count[kSilver] + earned_count[kGold] + platinum;

  const ImVec2 display = io.DisplaySize;
  const float scale = display.y / 1080.0f;
  const float font_scale = scale * kFontScaleAt1080;

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f * scale, 6.0f * scale));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##grfs_trophies", nullptr, kFlags)) {
    ImDrawList* window_draw = ImGui::GetWindowDrawList();
    window_draw->AddRectFilledMultiColor(ImVec2(0, 0), display, kBackgroundTop, kBackgroundTop,
                                         kBackgroundBottom, kBackgroundBottom);
    if (font_) {
      ImGui::PushFont(font_);
    }
    ImGui::SetWindowFontScale(font_scale);

    const float left = display.x * 0.12f;
    const float width = display.x * 0.76f;

    // Header: the screen, the game, its progress and the trophy counts.
    ImGui::SetCursorPos(ImVec2(left, 56.0f * scale));
    ImGui::TextColored(kDimText, "%s", texts.title);
    ImGui::SetCursorPosX(left);
    if (title_font_) {
      ImGui::PushFont(title_font_);
    }
    ImGui::SetWindowFontScale(font_scale * 0.72f);
    ImGui::TextColored(kText, "%s", texts.game);
    ImGui::SetWindowFontScale(font_scale);

    ImGui::SetCursorPosX(left);
    const ImVec2 header = ImGui::GetCursorScreenPos();
    ImGui::TextColored(kText, "%d%%", percent);
    if (title_font_) {
      ImGui::PopFont();
    }
    ImGui::SetWindowFontScale(font_scale);
    const float bar_x = header.x + 150.0f * scale, bar_y = header.y + 20.0f * scale;
    const float bar_w = 360.0f * scale, bar_h = 6.0f * scale;
    window_draw->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h),
                               kBarBack, bar_h);
    window_draw->AddRectFilled(ImVec2(bar_x, bar_y),
                               ImVec2(bar_x + bar_w * percent / 100.0f, bar_y + bar_h), kBarFill,
                               bar_h);
    char earned_text[64];
    std::snprintf(earned_text, sizeof(earned_text), texts.earned_format, earned_total, count);
    window_draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                         ImVec2(bar_x, bar_y + 14.0f * scale), ImGui::GetColorU32(kDimText),
                         earned_text);
    float counts_x = left + width;
    for (int grade = kBronze; grade <= kPlatinum; ++grade) {
      std::string text = std::to_string(earned_count[grade]);
      counts_x -= ImGui::CalcTextSize(text.c_str()).x;
      window_draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                           ImVec2(counts_x, header.y + 8.0f * scale), ImGui::GetColorU32(kText),
                           text.c_str());
      counts_x -= 30.0f * scale;
      DrawTrophy(window_draw, ImVec2(counts_x + 12.0f * scale, header.y + 26.0f * scale),
                 32.0f * scale, Grade(grade));
      counts_x -= 40.0f * scale;
    }
    ImGui::Dummy(ImVec2(0, 20.0f * scale));

    // The trophy list.
    const float footer = 90.0f * scale;
    ImGui::SetCursorPosX(left);
    ImGui::BeginChild("##list", ImVec2(width, display.y - ImGui::GetCursorPosY() - footer),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(font_scale);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float icon = 88.0f * scale, pad = 16.0f * scale, round = 10.0f * scale;
    const float edge = 3.0f * scale;
    for (int i = 0; i < count; ++i) {
      const bool is_platinum = i == 0;
      const rex::system::AchievementInfo* achievement = is_platinum ? nullptr : &list[i - 1];
      const bool earned = is_platinum ? platinum : achievements_->IsUnlocked(achievement->id);
      const Grade grade = is_platinum ? kPlatinum : GradeOf(achievement->gamerscore);
      const bool focused = i == selected_;

      ImGui::PushID(i);
      ImGui::SetCursorPosX(edge);
      const ImVec2 row = ImGui::GetCursorScreenPos();
      const float row_width = width - edge * 2.0f;
      const float text_x = row.x + pad + icon + 24.0f * scale;
      const float right = row.x + row_width - pad;
      const float grade_column = 150.0f * scale;
      const float wrap = right - grade_column - text_x;

      const std::string name = is_platinum ? texts.game : achievement->label;
      const std::string description =
          is_platinum ? texts.platinum_description
                      : (earned || achievement->unachieved_description.empty()
                             ? achievement->description
                             : achievement->unachieved_description);
      ImGui::SetWindowFontScale(font_scale * 0.85f);
      const float description_size = ImGui::GetFontSize();
      const float description_height =
          ImGui::CalcTextSize(description.c_str(), nullptr, false, wrap).y;
      ImGui::SetWindowFontScale(font_scale);
      const float name_size = title_font_ ? title_font_->LegacySize * font_scale * 0.62f
                                          : ImGui::GetFontSize();
      const float text_height = name_size * 1.35f + description_height;
      const float row_height = std::max(icon, text_height) + pad * 2.0f;
      const ImVec2 row_end(row.x + row_width, row.y + row_height);

      draw->AddRectFilled(row, row_end, focused ? kRowFocused : kRow, round);
      if (focused) {
        draw->AddRect(ImVec2(row.x - edge * 0.5f, row.y - edge * 0.5f),
                      ImVec2(row_end.x + edge * 0.5f, row_end.y + edge * 0.5f), kFocusEdge,
                      round + edge, 0, edge);
      }

      // Icon: the achievement's own, dimmed while locked; the platinum's cup.
      const ImVec2 icon_min(row.x + pad, row.y + (row_height - icon) * 0.5f);
      const ImVec2 icon_max(icon_min.x + icon, icon_min.y + icon);
      if (is_platinum) {
        draw->AddRectFilled(icon_min, icon_max, IM_COL32(255, 255, 255, 18), 8.0f * scale);
        DrawTrophy(draw, ImVec2(icon_min.x + icon * 0.5f, icon_min.y + icon * 0.5f), icon * 0.7f,
                   kPlatinum, earned);
      } else if (rex::ui::ImmediateTexture* texture = icon_cache_.GetIcon(*achievement)) {
        draw->AddImageRounded(reinterpret_cast<ImTextureID>(texture), icon_min, icon_max,
                              ImVec2(0, 0), ImVec2(1, 1),
                              earned ? IM_COL32_WHITE : IM_COL32(110, 110, 120, 200),
                              8.0f * scale);
      }

      // Name and description.
      const float text_y = row.y + (row_height - text_height) * 0.5f;
      draw->AddText(title_font_ ? title_font_ : ImGui::GetFont(), name_size,
                    ImVec2(text_x, text_y), ImGui::GetColorU32(earned ? kText : kLockedText),
                    name.c_str());
      draw->AddText(ImGui::GetFont(), description_size,
                    ImVec2(text_x, text_y + name_size * 1.35f), ImGui::GetColorU32(kDimText),
                    description.c_str(), nullptr, wrap);

      // Grade, and the date it was earned.
      const float grade_x = right - grade_column * 0.5f;
      DrawTrophy(draw, ImVec2(grade_x, row.y + row_height * 0.5f - 14.0f * scale), 40.0f * scale,
                 grade, earned);
      std::string detail =
          earned && !is_platinum
              ? FormatDate(achievements_->GetUnlockTime(achievement->id), texts.date_format)
              : std::string(texts.grades[grade]);
      const float detail_size = ImGui::GetFontSize() * 0.75f;
      const float detail_width =
          ImGui::GetFont()->CalcTextSizeA(detail_size, FLT_MAX, 0.0f, detail.c_str()).x;
      draw->AddText(ImGui::GetFont(), detail_size,
                    ImVec2(grade_x - detail_width * 0.5f, row.y + row_height * 0.5f + 16.0f * scale),
                    ImGui::GetColorU32(kDimText), detail.c_str());

      ImGui::Dummy(ImVec2(row_width, row_height));
      if (scroll_to_ == i) {
        ImGui::SetScrollHereY(0.5f);
        scroll_to_ = -1;
      }
      ImGui::Dummy(ImVec2(0, 4.0f * scale));
      ImGui::PopID();
    }
    ImGui::EndChild();

    // Button hints, bottom right.
    ImGui::SetWindowFontScale(font_scale * 0.9f);
    const float hint_y = display.y - footer * 0.55f;
    const float glyph = 15.0f * scale;
    float x = left + width;
    auto hint = [&](const char* label, bool circle) {
      x -= ImGui::CalcTextSize(label).x;
      window_draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                           ImVec2(x, hint_y - ImGui::GetFontSize() * 0.55f),
                           ImGui::GetColorU32(kText), label);
      x -= glyph + 12.0f * scale;
      if (circle) {
        DrawCircleButton(window_draw, ImVec2(x, hint_y), glyph);
      } else {
        DrawDpad(window_draw, ImVec2(x, hint_y), glyph);
      }
      x -= glyph + 40.0f * scale;
    };
    hint(texts.back, true);
    hint(texts.scroll, false);

    if (font_) {
      ImGui::PopFont();
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}

AchievementToast::AchievementToast(rex::ui::ImGuiDrawer* imgui_drawer,
                                   rex::ui::ImmediateDrawer* immediate_drawer,
                                   rex::Runtime* runtime, ImFont* font, ImFont* title_font)
    : AchievementNotificationDialog(imgui_drawer),
      icon_cache_(immediate_drawer, runtime),
      font_(font),
      title_font_(title_font) {}

void AchievementToast::Push(const rex::system::AchievementEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back(event.achievement);
}

void AchievementToast::OnDraw(ImGuiIO& io) {
  rex::system::AchievementInfo achievement;
  float age = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    while (!queue_.empty()) {
      if (!shown_since_) {
        shown_since_ = now;
        if (GrfsApp* app = GrfsApp::instance()) {
          PlayTrophySound(app->GameFolder());
        }
      }
      age = std::chrono::duration<float>(now - *shown_since_).count();
      if (age < kDisplaySeconds) {
        break;
      }
      queue_.pop_front();
      shown_since_.reset();
    }
    if (queue_.empty()) {
      return;
    }
    achievement = queue_.front();
  }

  // A card at the top right, sliding in from the right edge and back.
  const Texts& texts = GetTexts();
  const ImVec2 display = io.DisplaySize;
  const float scale = display.y / 1080.0f;
  const float font_scale = scale * kFontScaleAt1080;
  const float shown =
      std::clamp(std::min(age / 0.3f, (kDisplaySeconds - age) / 0.3f), 0.0f, 1.0f);
  const float eased = 1.0f - (1.0f - shown) * (1.0f - shown);
  const ImVec2 size(580.0f * scale, 112.0f * scale);
  const float margin = 40.0f * scale;
  const ImVec2 card(display.x + (size.x + margin) * (1.0f - eased) - margin - size.x, margin);
  const ImVec2 card_max(card.x + size.x, card.y + size.y);

  ImDrawList* draw = ImGui::GetForegroundDrawList();
  draw->AddRectFilled(card, card_max, kCard, 14.0f * scale);

  const float pad = 16.0f * scale, icon = size.y - pad * 2.0f;
  const ImVec2 icon_min(card.x + pad, card.y + pad);
  if (rex::ui::ImmediateTexture* texture = icon_cache_.GetIcon(achievement)) {
    draw->AddImageRounded(reinterpret_cast<ImTextureID>(texture), icon_min,
                          ImVec2(icon_min.x + icon, icon_min.y + icon), ImVec2(0, 0),
                          ImVec2(1, 1), IM_COL32_WHITE, 8.0f * scale);
  }
  ImFont* font = font_ ? font_ : ImGui::GetFont();
  ImFont* title_font = title_font_ ? title_font_ : font;
  const float text_x = icon_min.x + icon + 20.0f * scale;
  const float small = font->LegacySize * font_scale * 0.8f;
  const float big = title_font->LegacySize * font_scale * 0.55f;
  const float text_y = card.y + (size.y - small * 1.3f - big) * 0.5f;
  DrawTrophy(draw, ImVec2(text_x + 12.0f * scale, text_y + small * 0.55f), small,
             GradeOf(achievement.gamerscore));
  draw->AddText(font, small, ImVec2(text_x + 34.0f * scale, text_y), ImGui::GetColorU32(kDimText),
                texts.notification);
  draw->AddText(title_font, big, ImVec2(text_x, text_y + small * 1.3f), ImGui::GetColorU32(kText),
                achievement.label.c_str());
}
