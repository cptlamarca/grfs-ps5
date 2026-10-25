// See iso_screen.h. Shown before the runtime exists, so the DualSense is read
// here directly (the runtime's input driver opens it again later).

#include "iso_screen.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <imgui.h>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "achievements_ui.h"
#include "third_party/qrcodegen/qrcodegen.hpp"

#if defined(__PROSPERO__)
extern "C" {
int sceUserServiceInitialize(void* params);
int sceUserServiceGetInitialUser(int32_t* user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void* param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void* data);
}
#endif

namespace {

constexpr char kDiscordLink[] = "discord.gg/pmK6zDYQSP";
constexpr char kDiscordUrl[] = "https://discord.gg/pmK6zDYQSP";
// Files the game's disc has at its root: the game and its AI module.
constexpr char kGameFile[] = "default.xex";
constexpr char kAiFile[] = "AICLASS_X360_F.xex";

// ScePadData's buttons (as src/input/ps5/ps5_input_driver.cpp of the SDK).
constexpr uint32_t kPadUp = 0x10, kPadDown = 0x40;
constexpr uint32_t kPadCircle = 0x2000, kPadCross = 0x4000, kPadSquare = 0x8000;

constexpr float kFontScaleAt1080 = 0.5f;
constexpr ImU32 kBackgroundTop = IM_COL32(16, 30, 54, 255);
constexpr ImU32 kBackgroundBottom = IM_COL32(4, 8, 16, 255);
constexpr ImVec4 kText = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
constexpr ImVec4 kDimText = ImVec4(0.70f, 0.73f, 0.78f, 1.0f);
constexpr ImVec4 kAccent = ImVec4(0.45f, 0.75f, 1.0f, 1.0f);
constexpr ImVec4 kError = ImVec4(1.0f, 0.45f, 0.40f, 1.0f);

// The folder as the console's storage has it: the app's own is mounted at
// /app0, from /data/homebrew/<title ID> (where FTP shows it) or a USB
// drive's homebrew folder; the one whose eboot.bin is the app's.
std::filesystem::path StorageFolder(const std::filesystem::path& folder) {
#if defined(__PROSPERO__)
  static const std::filesystem::path app_storage = [] {
    const std::filesystem::path app = "/app0";
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(app / "eboot.bin", ec);
    const auto time = std::filesystem::last_write_time(app / "eboot.bin", ec);
    if (ec) {
      return app;
    }
    std::vector<std::filesystem::path> roots = {"/data/homebrew"};
    for (int i = 0; i < 8; ++i) {
      roots.push_back("/mnt/usb" + std::to_string(i) + "/homebrew");
    }
    for (const std::filesystem::path& root : roots) {
      if (!std::filesystem::is_directory(root, ec)) {
        continue;
      }
      for (const auto& entry : std::filesystem::directory_iterator(
               root, std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::error_code entry_ec;
        const std::filesystem::path eboot = entry.path() / "eboot.bin";
        if (std::filesystem::file_size(eboot, entry_ec) == size && !entry_ec &&
            std::filesystem::last_write_time(eboot, entry_ec) == time && !entry_ec) {
          return entry.path();
        }
      }
    }
    return app;
  }();
  const std::string path = folder.string();
  if (path.compare(0, 5, "/app0") == 0 && (path.size() == 5 || path[5] == '/')) {
    return std::filesystem::path(app_storage.string() + path.substr(5));
  }
#endif
  return folder;
}

// Where disc images are looked for (and one folder down): the app's folder,
// the console's storage and USB drives.
std::vector<std::filesystem::path> SearchFolders() {
  const std::filesystem::path app = rex::filesystem::GetExecutableFolder();
  std::vector<std::filesystem::path> folders = {app};
#if defined(__PROSPERO__)
  folders.push_back(StorageFolder(app).parent_path());
  folders.push_back("/data");
  for (int i = 0; i < 8; ++i) {
    folders.push_back("/mnt/usb" + std::to_string(i));
  }
  folders.push_back("/mnt/ext0");
  folders.push_back("/mnt/ext1");
#endif
  return folders;
}

bool IsIso(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return extension == ".iso";
}

std::string Gigabytes(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f GB", double(bytes) / 1e9);
  return text;
}

enum class Glyph { kCross, kCircle, kSquare };

// A DualSense button: a white disc with its symbol.
void DrawGlyph(ImDrawList* draw, ImVec2 c, float r, Glyph glyph) {
  const ImU32 ink = IM_COL32(20, 24, 32, 255);
  draw->AddCircleFilled(c, r, IM_COL32(255, 255, 255, 235), 24);
  const float s = r * 0.45f, t = r * 0.17f;
  if (glyph == Glyph::kCross) {
    draw->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), ink, t);
    draw->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), ink, t);
  } else if (glyph == Glyph::kCircle) {
    draw->AddCircle(c, r * 0.5f, ink, 24, t);
  } else {
    draw->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), ink, 0.0f, 0, t);
  }
}

// Hints along the bottom, left to right from x.
void DrawHints(ImDrawList* draw, float x, float y, float scale,
               const std::vector<std::pair<Glyph, std::string>>& hints) {
  const float radius = 18.0f * scale;
  for (const auto& [glyph, label] : hints) {
    DrawGlyph(draw, ImVec2(x + radius, y + radius), radius, glyph);
    ImGui::SetCursorPos(ImVec2(x + radius * 2.0f + 12.0f * scale, y));
    ImGui::TextColored(kText, "%s", label.c_str());
    x += radius * 2.0f + 12.0f * scale + ImGui::CalcTextSize(label.c_str()).x + 50.0f * scale;
  }
}

void DrawQr(ImDrawList* draw, ImVec2 at, float size, float scale, const char* text) {
  try {
    qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text, qrcodegen::QrCode::Ecc::MEDIUM);
    // A white card with a quiet zone of 3 modules.
    const int modules = qr.getSize();
    const float module = size / float(modules + 6);
    draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size), IM_COL32(255, 255, 255, 255),
                        12.0f * scale);
    for (int y = 0; y < modules; ++y) {
      for (int x = 0; x < modules; ++x) {
        if (qr.getModule(x, y)) {
          ImVec2 corner(at.x + (x + 3) * module, at.y + (y + 3) * module);
          draw->AddRectFilled(corner, ImVec2(corner.x + module + 0.5f, corner.y + module + 0.5f),
                              IM_COL32(0, 0, 0, 255));
        }
      }
    }
  } catch (const std::exception&) {
  }
}

}  // namespace

bool IsoScreen::HasGameFiles(const std::filesystem::path& folder) {
  std::error_code ec;
  return std::filesystem::exists(folder / kGameFile, ec);
}

IsoScreen::IsoScreen(rex::ui::ImGuiDrawer* imgui_drawer, ImFont* font, ImFont* title_font,
                     std::filesystem::path game_folder, std::function<void()> done)
    : ImGuiDialog(imgui_drawer),
      font_(font),
      title_font_(title_font),
      game_folder_(std::move(game_folder)),
      done_(std::move(done)) {
#if defined(__PROSPERO__)
  sceUserServiceInitialize(nullptr);
  scePadInit();
#endif
  Search();
}

IsoScreen::~IsoScreen() {
  progress_.cancel = true;
  if (worker_.joinable()) {
    worker_.join();
  }
}

void IsoScreen::Search() {
  images_.clear();
  std::error_code ec;
  for (const std::filesystem::path& folder : SearchFolders()) {
    if (!std::filesystem::is_directory(folder, ec)) {
      continue;
    }
    std::vector<std::filesystem::path> folders = {folder};
    for (const auto& entry : std::filesystem::directory_iterator(
             folder, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (entry.is_directory(ec) && entry.path() != game_folder_) {
        folders.push_back(entry.path());
      }
    }
    for (const std::filesystem::path& where : folders) {
      for (const auto& entry : std::filesystem::directory_iterator(
               where, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (IsIso(entry.path()) && entry.is_regular_file(ec)) {
          Image image{entry.path(), entry.file_size(ec)};
          if (std::none_of(images_.begin(), images_.end(),
                           [&](const Image& i) { return i.path == image.path; })) {
            images_.push_back(image);
          }
        }
      }
    }
  }
  selected_ = std::min(selected_, std::max(int(images_.size()) - 1, 0));
  REXLOG_INFO("Disc images found: {} (app folder {})", images_.size(),
              StorageFolder(rex::filesystem::GetExecutableFolder()).string());
}

void IsoScreen::Extract(const Image& image) {
  state_ = State::kExtracting;
  progress_.done = 0;
  progress_.cancel = false;
  total_ = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_file_.clear();
    error_.clear();
  }
  worker_ = std::thread([this, image] {
    const bool portuguese = IsPortuguese();
    std::vector<DiscFile> files;
    std::string error;
    bool ok = ListDiscImage(image.path, files, error);
    if (ok && (!FindRootFile(files, kGameFile) || !FindRootFile(files, kAiFile))) {
      ok = false;
      error = portuguese ? "Esta ISO não é do Ghost Recon: Future Soldier."
                         : "This ISO isn't Ghost Recon: Future Soldier's.";
    }
    if (ok) {
      uint64_t total = 0;
      for (const DiscFile& file : files) {
        total += file.directory ? 0 : file.size;
      }
      total_ = total;
      std::error_code ec;
      std::filesystem::create_directories(game_folder_, ec);
      std::filesystem::space_info space = std::filesystem::space(game_folder_, ec);
      if (!ec && space.available < total + (256u << 20)) {
        ok = false;
        error = (portuguese ? "Espaço insuficiente: a extração precisa de "
                            : "Not enough space: the extraction needs ") +
                Gigabytes(total) + (portuguese ? ", livres: " : ", free: ") +
                Gigabytes(space.available) + ".";
      }
    }
    if (ok) {
      REXLOG_INFO("Extracting {} into {}", image.path.string(), game_folder_.string());
      ok = ExtractDiscImage(
          image.path, files, game_folder_, kGameFile, progress_,
          [this](const std::string& path) {
            std::lock_guard<std::mutex> lock(mutex_);
            current_file_ = path;
          },
          error);
    }
    if (!ok) {
      REXLOG_ERROR("Disc image {}: {}", image.path.string(), error);
      std::lock_guard<std::mutex> lock(mutex_);
      error_ = error;
    }
    state_ = ok ? State::kDone : State::kFailed;
  });
}

uint32_t IsoScreen::PollPad() {
  uint32_t buttons = 0;
#if defined(__PROSPERO__)
  if (pad_ < 0) {
    int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) == 0 && user >= 0) {
      pad_ = scePadOpen(user, 0, 0, nullptr);
      if (pad_ < 0) {
        pad_ = scePadGetHandle(user, 0, 0);
      }
    }
  }
  struct {
    uint32_t buttons;
    uint8_t left_stick_x, left_stick_y, right_stick_x, right_stick_y;
    uint8_t rest[248];
  } data;
  std::memset(&data, 0, sizeof(data));
  if (pad_ >= 0 && scePadReadState(pad_, &data) == 0) {
    buttons = data.buttons;
    if (data.left_stick_y < 40) {
      buttons |= kPadUp;
    } else if (data.left_stick_y > 215) {
      buttons |= kPadDown;
    }
  }
#endif
  // The keyboard too (and a PC's only input).
  if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) buttons |= kPadUp;
  if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) buttons |= kPadDown;
  if (ImGui::IsKeyDown(ImGuiKey_Enter)) buttons |= kPadCross;
  if (ImGui::IsKeyDown(ImGuiKey_Escape)) buttons |= kPadCircle;
  if (ImGui::IsKeyDown(ImGuiKey_F5)) buttons |= kPadSquare;
  uint32_t pressed = buttons & ~previous_buttons_;
  previous_buttons_ = buttons;
  return pressed;
}

void IsoScreen::OnDraw(ImGuiIO& io) {
  const uint32_t pressed = PollPad();
  const State state = state_;
  const bool portuguese = IsPortuguese();
  if (state == State::kChoosing) {
    if (pressed & kPadUp) {
      selected_ = std::max(selected_ - 1, 0);
    }
    if (pressed & kPadDown) {
      selected_ = std::max(std::min(selected_ + 1, int(images_.size()) - 1), 0);
    }
    if (pressed & kPadSquare) {
      Search();
    }
    if ((pressed & kPadCross) && selected_ < int(images_.size())) {
      if (worker_.joinable()) {
        worker_.join();
      }
      Extract(images_[selected_]);
    }
  } else if (state == State::kExtracting) {
    if (pressed & kPadCircle) {
      progress_.cancel = true;
    }
  } else if (state == State::kFailed) {
    if (pressed & (kPadCross | kPadCircle)) {
      if (worker_.joinable()) {
        worker_.join();
      }
      Search();
      state_ = State::kChoosing;
    }
  } else if (state == State::kDone && !done_called_) {
    done_called_ = true;
    if (worker_.joinable()) {
      worker_.join();
    }
    done_();
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
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                      ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##grfs_iso", nullptr, kFlags)) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilledMultiColor(ImVec2(0, 0), display, kBackgroundTop, kBackgroundTop,
                                  kBackgroundBottom, kBackgroundBottom);
    if (font_) {
      ImGui::PushFont(font_);
    }
    const float left = display.x * 0.08f;
    const float qr_size = display.y * 0.26f;
    const float qr_x = display.x * 0.92f - qr_size;
    const float right = qr_x - 70.0f * scale;
    const float top = display.y * 0.09f;

    // The title.
    if (title_font_) {
      ImGui::PushFont(title_font_);
    }
    ImGui::SetWindowFontScale(font_scale * 0.85f);
    ImGui::SetCursorPos(ImVec2(left, top));
    ImGui::TextColored(kText, "%s",
                       state == State::kExtracting || state == State::kDone
                           ? (portuguese ? "Extraindo os arquivos do jogo" : "Extracting the game files")
                           : (portuguese ? "Arquivos do jogo ausentes" : "Game files missing"));
    if (title_font_) {
      ImGui::PopFont();
    }

    // Discord, right.
    DrawQr(draw, ImVec2(qr_x, top), qr_size, scale, kDiscordUrl);
    ImGui::SetWindowFontScale(font_scale * 0.75f);
    ImGui::SetCursorPos(ImVec2(qr_x, top + qr_size + 20.0f * scale));
    ImGui::PushTextWrapPos(qr_x + qr_size);
    ImGui::TextColored(kDimText, "%s",
                       portuguese ? "Dúvidas ou problemas? Entre no Discord:"
                                  : "Questions or problems? Join the Discord:");
    ImGui::SetCursorPosX(qr_x);
    ImGui::TextColored(kAccent, "%s", kDiscordLink);
    ImGui::PopTextWrapPos();

    ImGui::SetWindowFontScale(font_scale * 0.8f);
    ImGui::SetCursorPos(ImVec2(left, top + 120.0f * scale));
    ImGui::PushTextWrapPos(right);
    const std::string app = StorageFolder(rex::filesystem::GetExecutableFolder()).string();
    const std::string game_folder = StorageFolder(game_folder_).string();
    if (state == State::kChoosing || state == State::kFailed) {
      ImGui::SetCursorPosX(left);
      ImGui::TextColored(kDimText, "%s",
                         portuguese ? "Os arquivos extraídos da ISO do Ghost Recon: Future Soldier "
                                      "(Xbox 360) não foram encontrados. Eles devem ficar nesta "
                                      "pasta, com o default.xex dentro:"
                                    : "The files extracted from Ghost Recon: Future Soldier's "
                                      "(Xbox 360) ISO weren't found. They go in this folder, with "
                                      "default.xex inside:");
      ImGui::SetCursorPosX(left);
      ImGui::TextColored(kAccent, "%s", game_folder.c_str());
      ImGui::SetCursorPos(ImVec2(left, ImGui::GetCursorPosY() + 24.0f * scale));
      ImGui::TextColored(
          kDimText, "%s",
          (portuguese ? "Ou coloque o arquivo .iso do jogo na pasta " + app +
                            " ou na raiz de um pendrive ou HD externo, escolha-o abaixo e "
                            "pressione X: os arquivos serão extraídos e o jogo vai iniciar."
                      : "Or put the game's .iso file in " + app +
                            " or at the root of a USB drive, choose it below and press X: its "
                            "files will be extracted and the game will start.")
              .c_str());
    }
    ImGui::PopTextWrapPos();

    const float list_top = ImGui::GetCursorPosY() + 40.0f * scale;
    const float row_height = 64.0f * scale;
    if (state == State::kChoosing) {
      ImGui::SetCursorPos(ImVec2(left, list_top));
      ImGui::TextColored(kText, "%s", portuguese ? "ISOs encontradas" : "ISOs found");
      float y = ImGui::GetCursorPosY() + 10.0f * scale;
      if (images_.empty()) {
        ImGui::SetCursorPos(ImVec2(left, y));
        ImGui::PushTextWrapPos(right);
        ImGui::TextColored(kDimText, "%s",
                           portuguese ? "Nenhuma ISO encontrada. Coloque uma e pressione Quadrado "
                                        "para procurar de novo."
                                      : "No ISO found. Put one there and press Square to search "
                                        "again.");
        ImGui::PopTextWrapPos();
      }
      // Up to 6 rows, scrolled to keep the selected one in.
      const int rows = 6;
      const int first = std::clamp(selected_ - rows + 1, 0, std::max(int(images_.size()) - rows, 0));
      for (int i = first; i < int(images_.size()) && i < first + rows; ++i) {
        const ImVec2 a(left, y), b(right, y + row_height - 8.0f * scale);
        const bool chosen = i == selected_;
        draw->AddRectFilled(a, b, chosen ? IM_COL32(60, 110, 180, 200) : IM_COL32(255, 255, 255, 18),
                            8.0f * scale);
        ImGui::SetCursorPos(ImVec2(left + 20.0f * scale, y + 10.0f * scale));
        ImGui::TextColored(kText, "%s", images_[i].path.filename().string().c_str());
        const std::string detail =
            images_[i].path.parent_path().string() + "   " + Gigabytes(images_[i].size);
        const float detail_width = ImGui::CalcTextSize(detail.c_str()).x;
        ImGui::SetCursorPos(ImVec2(std::max(right - 20.0f * scale - detail_width,
                                            left + display.x * 0.25f),
                                   y + 10.0f * scale));
        ImGui::TextColored(chosen ? kText : kDimText, "%s", detail.c_str());
        y += row_height;
      }
    } else if (state == State::kExtracting || state == State::kDone) {
      const uint64_t total = total_, done = std::min<uint64_t>(progress_.done, total);
      const float fraction = total ? float(double(done) / double(total)) : 0.0f;
      std::string file;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        file = current_file_;
      }
      ImGui::SetCursorPos(ImVec2(left, top + 140.0f * scale));
      ImGui::PushTextWrapPos(right);
      ImGui::TextColored(kDimText, "%s",
                         ((portuguese ? "Para a pasta " : "Into ") + game_folder).c_str());
      ImGui::TextColored(kDimText, "%s",
                         portuguese ? "Não feche o jogo nem desligue o console. O jogo inicia "
                                      "quando terminar."
                                    : "Don't close the game or turn the console off. The game "
                                      "starts when it's done.");
      ImGui::PopTextWrapPos();
      const float bar_y = ImGui::GetCursorPosY() + 50.0f * scale;
      const float bar_height = 36.0f * scale;
      draw->AddRectFilled(ImVec2(left, bar_y), ImVec2(right, bar_y + bar_height),
                          IM_COL32(255, 255, 255, 30), bar_height / 2);
      if (fraction > 0.0f) {
        draw->AddRectFilled(ImVec2(left, bar_y),
                            ImVec2(left + (right - left) * fraction, bar_y + bar_height),
                            IM_COL32(90, 160, 240, 255), bar_height / 2);
      }
      char line[96];
      std::snprintf(line, sizeof(line), "%d%%   %s / %s", int(fraction * 100.0f),
                    Gigabytes(done).c_str(), Gigabytes(total).c_str());
      ImGui::SetCursorPos(ImVec2(left, bar_y + bar_height + 20.0f * scale));
      ImGui::TextColored(kText, "%s", line);
      ImGui::SetCursorPosX(left);
      ImGui::PushTextWrapPos(right);
      ImGui::TextColored(kDimText, "%s", file.c_str());
      ImGui::PopTextWrapPos();
    } else if (state == State::kFailed) {
      std::string error;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        error = error_;
      }
      ImGui::SetCursorPos(ImVec2(left, list_top));
      ImGui::PushTextWrapPos(right);
      ImGui::TextColored(kError, "%s",
                         ((portuguese ? "Não foi possível extrair a ISO: " : "Couldn't extract the ISO: ") +
                          error)
                             .c_str());
      ImGui::PopTextWrapPos();
    }

    ImGui::SetWindowFontScale(font_scale * 0.8f);
    const float hint_y = display.y - 90.0f * scale;
    if (state == State::kChoosing) {
      std::vector<std::pair<Glyph, std::string>> hints;
      if (!images_.empty()) {
        hints.emplace_back(Glyph::kCross, portuguese ? "Extrair e jogar" : "Extract and play");
      }
      hints.emplace_back(Glyph::kSquare, portuguese ? "Procurar de novo" : "Search again");
      DrawHints(draw, left, hint_y, scale, hints);
    } else if (state == State::kExtracting) {
      DrawHints(draw, left, hint_y, scale, {{Glyph::kCircle, portuguese ? "Cancelar" : "Cancel"}});
    } else if (state == State::kFailed) {
      DrawHints(draw, left, hint_y, scale, {{Glyph::kCross, portuguese ? "Voltar" : "Back"}});
    }
    if (font_) {
      ImGui::PopFont();
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}
