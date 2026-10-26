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
#include "iso_screen.h"
#include "screen_style.h"
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
  const char* kicker;
  const char* earned;
  const char* locked;
  const char* last;
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
                                 "%m/%d/%Y",
                                 "TROPHIES  //  GHOST RECON",
                                 "EARNED",
                                 "LOCKED",
                                 "last earned"};
  static const Texts kPortuguese = {"Troféus",
                                    "Tom Clancy's Ghost Recon: Future Soldier",
                                    "%d/%d troféus",
                                    {"Bronze", "Prata", "Ouro", "Platina"},
                                    "Ganhe todos os outros troféus",
                                    "Voltar",
                                    "Rolar",
                                    "Você ganhou um troféu.",
                                    "%d/%m/%Y",
                                    "TROFÉUS  //  GHOST RECON",
                                    "CONQUISTADOS",
                                    "BLOQUEADOS",
                                    "último em"};
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

constexpr ImU32 kCard = IM_COL32(32, 34, 40, 242);
constexpr ImVec4 kText = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
constexpr ImVec4 kDimText = ImVec4(0.70f, 0.73f, 0.78f, 1.0f);
constexpr ImVec4 kLockedText = ImVec4(0.62f, 0.66f, 0.71f, 1.0f);


// A padlock: its shackle and body, over a locked trophy's icon.
void DrawPadlock(ImDrawList* draw, ImVec2 center, float size, ImU32 color) {
  const float w = size * 0.5f, body_top = center.y - size * 0.05f;
  draw->PathArcTo(ImVec2(center.x, body_top), w * 0.62f, 3.14159265f, 6.2831853f, 20);
  draw->PathStroke(color, 0, size * 0.13f);
  draw->AddRectFilled(ImVec2(center.x - w, body_top), ImVec2(center.x + w, center.y + size * 0.55f),
                      color, size * 0.08f);
  draw->AddCircleFilled(ImVec2(center.x, body_top + size * 0.25f), size * 0.08f,
                        IM_COL32(20, 24, 32, 255), 12);
}

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
      title_font_(title_font) {
  background_ = CreateSkullTexture(immediate_drawer, background_aspect_);
}

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

  uint16_t pressed = PollPad();
  if (pressed == 0xFFFF) {
    // Hidden once the button that closed it is up, so the game doesn't get it.
    app_->SetAchievementsOverlayVisible(false);
    return;
  }
  if (pressed & (rex::input::X_INPUT_GAMEPAD_B | rex::input::X_INPUT_GAMEPAD_BACK)) {
    closing_ = true;
  }

  int earned_count[4] = {}, total_count[4] = {};
  int earned_points = 0, total_points = 0;
  uint64_t last_unlock = 0;
  for (const auto& achievement : list) {
    Grade grade = GradeOf(achievement.gamerscore);
    ++total_count[grade];
    total_points += kGradePoints[grade];
    if (achievements_->IsUnlocked(achievement.id)) {
      ++earned_count[grade];
      earned_points += kGradePoints[grade];
      last_unlock = std::max(last_unlock, achievements_->GetUnlockTime(achievement.id));
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

  // The trophies earned first, the latest on top (the platinum above them
  // all), then those still locked (the platinum first).
  struct Entry {
    const rex::system::AchievementInfo* achievement;  // null: the platinum
    bool earned;
    Grade grade;
    uint64_t time;
  };
  std::vector<Entry> entries;
  entries.push_back({nullptr, platinum, kPlatinum, platinum ? ~uint64_t(0) : 0});
  for (const auto& achievement : list) {
    const bool earned = achievements_->IsUnlocked(achievement.id);
    entries.push_back({&achievement, earned, GradeOf(achievement.gamerscore),
                       earned ? achievements_->GetUnlockTime(achievement.id) : 0});
  }
  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.earned != b.earned) {
      return a.earned;
    }
    return a.earned && a.time > b.time;
  });
  const int count = int(entries.size());
  if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_UP) {
    selected_ = std::max(selected_ - 1, 0);
  }
  if (pressed & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) {
    selected_ = std::min(selected_ + 1, count - 1);
  }

  const ImVec2 display = io.DisplaySize;
  const float scale = display.y / 1080.0f;
  const float font_scale = scale * kFontScaleAt1080;
  const double now = ImGui::GetTime();
  const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
  if (shown_at_ < 0.0) {
    shown_at_ = now;
  }
  const float t = float(now - shown_at_);
  const float appear = EaseOut(std::clamp(t / 0.6f, 0.0f, 1.0f));
  const float ring_fill = EaseOut(std::clamp((t - 0.2f) / 1.2f, 0.0f, 1.0f));

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f * scale, 6.0f * scale));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##grfs_trophies", nullptr, kFlags)) {
    ImDrawList* window_draw = ImGui::GetWindowDrawList();
    DrawSkullBackground(window_draw, background_.get(), background_aspect_, display, t, scale,
                        SkullShade::kAll);
    if (font_) {
      ImGui::PushFont(font_);
    }
    ImGui::SetWindowFontScale(font_scale);

    const float left = display.x * 0.10f;
    const float width = display.x * 0.80f;
    const float slide = (1.0f - appear) * -60.0f * scale;

    // Header: the progress ring, the screen and the game, the trophy counts.
    const float top = 56.0f * scale;
    const float ring_radius = 62.0f * scale;
    const ImVec2 ring(left + ring_radius + slide, top + ring_radius + 8.0f * scale);
    {
      const float thickness = 12.0f * scale;
      window_draw->AddCircle(ring, ring_radius, IM_COL32(255, 255, 255, int(40 * appear)), 64,
                             thickness);
      const float fraction = percent / 100.0f * ring_fill;
      if (fraction > 0.0f) {
        const float start = -1.5707963f;
        window_draw->PathArcTo(ring, ring_radius, start, start + fraction * 6.2831853f, 64);
        window_draw->PathStroke(IM_COL32(90, 200, 255, int(255 * appear)), 0, thickness);
        // A soft glow at the arc's end.
        const float end = start + fraction * 6.2831853f;
        const ImVec2 tip(ring.x + std::cos(end) * ring_radius, ring.y + std::sin(end) * ring_radius);
        window_draw->AddCircleFilled(tip, thickness * 1.2f, IM_COL32(120, 220, 255, int(60 * appear)),
                                     24);
      }
      char text[16];
      std::snprintf(text, sizeof(text), "%d%%", int(percent * ring_fill + 0.5f));
      ImFont* font = title_font_ ? title_font_ : ImGui::GetFont();
      const float size = (title_font_ ? title_font_->LegacySize : ImGui::GetFontSize()) * font_scale *
                         0.62f;
      const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
      window_draw->AddText(font, size, ImVec2(ring.x - extent.x / 2, ring.y - extent.y / 2),
                           IM_COL32(255, 255, 255, int(255 * appear)), text);
    }
    const float header_x = ring.x + ring_radius + 36.0f * scale;
    ImGui::SetCursorPos(ImVec2(header_x + slide * 0.5f, top + 6.0f * scale));
    ImGui::SetWindowFontScale(font_scale * 0.7f);
    ImGui::TextColored(WithAlpha(kAccent, appear), "%s", texts.kicker);
    if (title_font_) {
      ImGui::PushFont(title_font_);
    }
    ImGui::SetWindowFontScale(font_scale * 0.72f);
    ImGui::SetCursorPosX(header_x + slide);
    ImGui::TextColored(WithAlpha(kText, appear), "%s", texts.game);
    if (title_font_) {
      ImGui::PopFont();
    }
    ImGui::SetWindowFontScale(font_scale * 0.8f);
    ImGui::SetCursorPosX(header_x + slide * 0.5f);
    char earned_text[64];
    std::snprintf(earned_text, sizeof(earned_text), texts.earned_format, earned_total, count);
    std::string summary = earned_text;
    if (last_unlock) {
      summary += "   ·   " + std::string(texts.last) + " " +
                 FormatDate(last_unlock, texts.date_format);
    }
    ImGui::TextColored(WithAlpha(kDimText, appear), "%s", summary.c_str());

    // The counts, right: a badge per grade.
    {
      float x = left + width;
      const float badge_h = 52.0f * scale, badge_y = ring.y - badge_h / 2;
      for (int grade = kPlatinum; grade >= kBronze; --grade) {
        const std::string text =
            std::to_string(earned_count[grade]) + "/" + std::to_string(total_count[grade]);
        const float text_w = ImGui::CalcTextSize(text.c_str()).x;
        const float badge_w = text_w + 76.0f * scale;
        x -= badge_w;
        const float a = EaseOut(std::clamp((t - 0.15f - 0.08f * (kPlatinum - grade)) / 0.4f, 0.0f, 1.0f));
        const ImU32 color = kGradeColors[grade];
        window_draw->AddRectFilled(ImVec2(x, badge_y), ImVec2(x + badge_w, badge_y + badge_h),
                                   IM_COL32(255, 255, 255, int(16 * a)), badge_h / 2);
        window_draw->AddRect(ImVec2(x, badge_y), ImVec2(x + badge_w, badge_y + badge_h),
                             (color & 0x00FFFFFF) | (ImU32(int(110 * a)) << 24), badge_h / 2, 0,
                             2.0f * scale);
        DrawTrophy(window_draw, ImVec2(x + 30.0f * scale, badge_y + badge_h / 2), 30.0f * scale,
                   Grade(grade), earned_count[grade] > 0);
        window_draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                             ImVec2(x + 54.0f * scale, badge_y + (badge_h - ImGui::GetFontSize()) / 2),
                             IM_COL32(255, 255, 255, int(255 * a)), text.c_str());
        x -= 14.0f * scale;
      }
    }

    // The list.
    const float list_top = ring.y + ring_radius + 36.0f * scale;
    const float footer = 90.0f * scale;
    const float list_height = display.y - list_top - footer;
    ImGui::SetCursorPos(ImVec2(left, list_top));
    ImGui::BeginChild("##list", ImVec2(width, list_height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(font_scale);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float icon = 88.0f * scale, pad = 18.0f * scale, round = 10.0f * scale;
    const float edge = 3.0f * scale;
    const float pulse = 0.5f + 0.5f * std::sin(t * 3.2f);
    float focus_top = 0.0f, focus_height = 0.0f;
    int shown_earned = earned_total, shown_locked = count - earned_total;
    for (int i = 0; i < count; ++i) {
      const Entry& entry = entries[i];
      const bool is_platinum = entry.achievement == nullptr;
      const bool earned = entry.earned;
      const Grade grade = entry.grade;
      const bool focused = i == selected_;
      const float row_alpha =
          EaseOut(std::clamp((t - 0.25f - 0.05f * std::min(i, 10)) / 0.4f, 0.0f, 1.0f));

      // A section's title before its first trophy.
      if (i == 0 || entries[i - 1].earned != earned) {
        ImGui::Dummy(ImVec2(0, (i == 0 ? 2.0f : 22.0f) * scale));
        ImGui::SetCursorPosX(edge);
        ImGui::SetWindowFontScale(font_scale * 0.65f);
        char section[64];
        std::snprintf(section, sizeof(section), "%s  (%d)", earned ? texts.earned : texts.locked,
                      earned ? shown_earned : shown_locked);
        ImGui::TextColored(WithAlpha(earned ? kAccent : kDimText, row_alpha), "%s", section);
        const ImVec2 rule = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(ImVec2(rule.x + edge, rule.y + 2.0f * scale),
                            ImVec2(rule.x + width - edge * 2, rule.y + 3.0f * scale),
                            IM_COL32(255, 255, 255, int(30 * row_alpha)));
        ImGui::Dummy(ImVec2(0, 10.0f * scale));
        ImGui::SetWindowFontScale(font_scale);
      }

      ImGui::PushID(i);
      ImGui::SetCursorPosX(edge);
      const ImVec2 row = ImGui::GetCursorScreenPos();
      const float row_y = ImGui::GetCursorPosY();
      const float row_width = width - edge * 2.0f;
      const float shift = (1.0f - row_alpha) * 40.0f * scale;
      const float text_x = row.x + pad + icon + 26.0f * scale + shift;
      const float right = row.x + row_width - pad;
      const float grade_column = 160.0f * scale;
      const float wrap = right - grade_column - text_x;

      const std::string name = is_platinum ? texts.game : entry.achievement->label;
      const std::string description =
          is_platinum ? texts.platinum_description
                      : (earned || entry.achievement->unachieved_description.empty()
                             ? entry.achievement->description
                             : entry.achievement->unachieved_description);
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
      if (focused) {
        focus_top = row_y;
        focus_height = row_height;
      }

      // The row: earned ones tinted with their grade's colour from the left.
      const ImU32 grade_color = kGradeColors[grade];
      const auto tint = [&](int alpha) {
        return (grade_color & 0x00FFFFFF) | (ImU32(std::clamp(alpha, 0, 255)) << 24);
      };
      draw->AddRectFilled(row, row_end,
                          IM_COL32(8, 14, 20, int((earned ? 190 : 150) * row_alpha)), round);
      if (earned) {
        draw->AddRectFilledMultiColor(row, ImVec2(row.x + row_width * 0.55f, row_end.y),
                                      tint(int(55 * row_alpha)), tint(0), tint(0),
                                      tint(int(55 * row_alpha)));
        draw->AddRectFilled(row, ImVec2(row.x + 6.0f * scale, row_end.y), tint(int(255 * row_alpha)),
                            round, ImDrawFlags_RoundCornersLeft);
      }
      if (focused) {
        draw->AddRectFilled(row, row_end, IM_COL32(255, 255, 255, 16), round);
        draw->AddRect(ImVec2(row.x - edge * 0.5f, row.y - edge * 0.5f),
                      ImVec2(row_end.x + edge * 0.5f, row_end.y + edge * 0.5f),
                      IM_COL32(110, 210, 255, int(150 + 105 * pulse)), round + edge, 0, edge);
      }

      // Icon: the achievement's own, dimmed with a padlock while locked; the
      // platinum's cup.
      const ImVec2 icon_min(row.x + pad + 8.0f * scale + shift, row.y + (row_height - icon) * 0.5f);
      const ImVec2 icon_max(icon_min.x + icon, icon_min.y + icon);
      const ImVec2 icon_center((icon_min.x + icon_max.x) / 2, (icon_min.y + icon_max.y) / 2);
      if (earned) {
        // A soft halo of its grade's colour, breathing.
        for (int ring_i = 3; ring_i >= 1; --ring_i) {
          draw->AddCircleFilled(icon_center, icon * (0.50f + 0.09f * ring_i) * (0.97f + 0.06f * pulse),
                                tint(int(14 * row_alpha)), 40);
        }
      }
      if (is_platinum) {
        draw->AddRectFilled(icon_min, icon_max, IM_COL32(255, 255, 255, int(18 * row_alpha)),
                            8.0f * scale);
        DrawTrophy(draw, icon_center, icon * 0.7f, kPlatinum, earned);
      } else if (rex::ui::ImmediateTexture* texture = icon_cache_.GetIcon(*entry.achievement)) {
        draw->AddImageRounded(reinterpret_cast<ImTextureID>(texture), icon_min, icon_max,
                              ImVec2(0, 0), ImVec2(1, 1),
                              earned ? IM_COL32(255, 255, 255, int(255 * row_alpha))
                                     : IM_COL32(90, 95, 105, int(170 * row_alpha)),
                              8.0f * scale);
      }
      if (!earned) {
        DrawPadlock(draw, icon_center, icon * 0.34f, IM_COL32(230, 235, 240, int(220 * row_alpha)));
      }

      // Name and description.
      const float text_y = row.y + (row_height - text_height) * 0.5f;
      draw->AddText(title_font_ ? title_font_ : ImGui::GetFont(), name_size,
                    ImVec2(text_x, text_y),
                    ImGui::GetColorU32(WithAlpha(earned ? kText : kLockedText, row_alpha)),
                    name.c_str());
      draw->AddText(ImGui::GetFont(), description_size,
                    ImVec2(text_x, text_y + name_size * 1.35f),
                    ImGui::GetColorU32(WithAlpha(kDimText, row_alpha)), description.c_str(),
                    nullptr, wrap);

      // Grade, and the date it was earned.
      const float grade_x = right - grade_column * 0.5f;
      DrawTrophy(draw, ImVec2(grade_x, row.y + row_height * 0.5f - 14.0f * scale), 40.0f * scale,
                 grade, earned);
      std::string detail =
          earned && !is_platinum
              ? FormatDate(entry.time, texts.date_format)
              : std::string(texts.grades[grade]);
      const float detail_size = ImGui::GetFontSize() * 0.75f;
      const float detail_width =
          ImGui::GetFont()->CalcTextSizeA(detail_size, FLT_MAX, 0.0f, detail.c_str()).x;
      draw->AddText(ImGui::GetFont(), detail_size,
                    ImVec2(grade_x - detail_width * 0.5f, row.y + row_height * 0.5f + 16.0f * scale),
                    ImGui::GetColorU32(WithAlpha(earned ? kText : kDimText, row_alpha)),
                    detail.c_str());

      ImGui::Dummy(ImVec2(row_width, row_height));
      ImGui::Dummy(ImVec2(0, 4.0f * scale));
      ImGui::PopID();
    }
    // Smooth scrolling to keep the focused trophy in the middle.
    {
      const float max_scroll = ImGui::GetScrollMaxY();
      const float target =
          std::clamp(focus_top + focus_height / 2 - list_height / 2, 0.0f, std::max(max_scroll, 0.0f));
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
                                           ImVec2(left + width, list_top + list_height), clear, clear,
                                           opaque, opaque);
    }

    // Button hints, bottom right.
    ImGui::SetWindowFontScale(font_scale * 0.9f);
    const float hint_y = display.y - footer * 0.55f;
    const float glyph = 15.0f * scale;
    float x = left + width;
    auto hint = [&](const char* label, bool circle) {
      x -= ImGui::CalcTextSize(label).x;
      window_draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                           ImVec2(x, hint_y - ImGui::GetFontSize() * 0.55f),
                           ImGui::GetColorU32(WithAlpha(kText, appear)), label);
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
    // Fading in from black.
    if (appear < 1.0f) {
      ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), display,
                                                    IM_COL32(0, 0, 0, int(200 * (1.0f - appear))));
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
