// The port's screens' look: the accent colour, fades and easing, and the
// HUD's corner brackets (with the skull art of iso_screen.h behind them).
#pragma once

#include <imgui.h>

constexpr ImVec4 kAccent = ImVec4(0.40f, 0.80f, 1.0f, 1.0f);

inline ImVec4 WithAlpha(ImVec4 color, float alpha) {
  color.w *= alpha;
  return color;
}

inline float EaseOut(float x) { return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x); }

// Corner brackets about a rectangle, their arms growing with `appear`.
inline void DrawHudCorners(ImDrawList* draw, ImVec2 a, ImVec2 b, float scale, float appear) {
  const float arm = 46.0f * scale * appear, thickness = 3.0f * scale;
  const ImU32 color = IM_COL32(90, 200, 255, int(200 * appear));
  for (int corner = 0; corner < 4; ++corner) {
    const float x = corner & 1 ? b.x : a.x, y = corner & 2 ? b.y : a.y;
    const float dx = corner & 1 ? -arm : arm, dy = corner & 2 ? -arm : arm;
    draw->AddLine(ImVec2(x, y), ImVec2(x + dx, y), color, thickness);
    draw->AddLine(ImVec2(x, y), ImVec2(x, y + dy), color, thickness);
  }
}
