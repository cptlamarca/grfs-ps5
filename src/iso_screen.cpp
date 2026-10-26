// See iso_screen.h. Shown before the runtime exists, so the DualSense is read
// here directly (the runtime's input driver opens it again later).

#include "iso_screen.h"
#include "screen_style.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>

#include <imgui.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/image_decode.h>

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
// The payload loader (etaHEN, elfldr, ...) and the extractor payload's files.
constexpr uint16_t kPayloadPort = 9021;
constexpr char kPayloadFolder[] = "/data/grfs-iso";

// ScePadData's buttons (as src/input/ps5/ps5_input_driver.cpp of the SDK).
constexpr uint32_t kPadUp = 0x10, kPadDown = 0x40;
constexpr uint32_t kPadCircle = 0x2000, kPadCross = 0x4000, kPadSquare = 0x8000;

constexpr float kFontScaleAt1080 = 0.5f;
constexpr ImVec4 kText = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
constexpr ImVec4 kDimText = ImVec4(0.74f, 0.80f, 0.84f, 1.0f);
constexpr ImVec4 kError = ImVec4(1.0f, 0.62f, 0.58f, 1.0f);


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

void DrawQr(ImDrawList* draw, ImVec2 at, float size, float scale, const char* text, float alpha) {
  try {
    qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text, qrcodegen::QrCode::Ecc::MEDIUM);
    // A white card with a quiet zone of 3 modules.
    const int modules = qr.getSize();
    const float module = size / float(modules + 6);
    draw->AddRectFilled(at, ImVec2(at.x + size, at.y + size),
                        IM_COL32(255, 255, 255, int(255 * alpha)), 10.0f * scale);
    for (int y = 0; y < modules; ++y) {
      for (int x = 0; x < modules; ++x) {
        if (qr.getModule(x, y)) {
          ImVec2 corner(at.x + (x + 3) * module, at.y + (y + 3) * module);
          draw->AddRectFilled(corner, ImVec2(corner.x + module + 0.5f, corner.y + module + 0.5f),
                              IM_COL32(0, 0, 0, int(255 * alpha)));
        }
      }
    }
  } catch (const std::exception&) {
  }
}

}  // namespace

// The wallpaper, built into the app (see CMakeLists.txt).
__asm__(".section .rodata\n"
        ".balign 16\n"
        ".hidden grfs_iso_background\n"
        ".globl grfs_iso_background\n"
        "grfs_iso_background:\n"
        ".incbin \"" GRFS_ISO_BACKGROUND "\"\n"
        ".hidden grfs_iso_background_end\n"
        ".globl grfs_iso_background_end\n"
        "grfs_iso_background_end:\n"
        ".previous\n");
// The extractor payload (payload/iso_extractor.cpp), built with the app.
__asm__(".section .rodata\n"
        ".balign 16\n"
        ".hidden grfs_iso_payload\n"
        ".globl grfs_iso_payload\n"
        "grfs_iso_payload:\n"
        ".incbin \"" GRFS_ISO_PAYLOAD "\"\n"
        ".hidden grfs_iso_payload_end\n"
        ".globl grfs_iso_payload_end\n"
        "grfs_iso_payload_end:\n"
        ".previous\n");
extern "C" const uint8_t grfs_iso_payload[];
extern "C" const uint8_t grfs_iso_payload_end[];
extern "C" const uint8_t grfs_iso_background[];
extern "C" const uint8_t grfs_iso_background_end[];

std::unique_ptr<rex::ui::ImmediateTexture> CreateSkullTexture(
    rex::ui::ImmediateDrawer* immediate_drawer, float& aspect) {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels = rex::ui::DecodeImageRGBA(
      grfs_iso_background, size_t(grfs_iso_background_end - grfs_iso_background), width, height);
  if (!immediate_drawer || pixels.empty()) {
    return nullptr;
  }
  aspect = float(width) / float(height);
  return immediate_drawer->CreateTexture(uint32_t(width), uint32_t(height),
                                         rex::ui::ImmediateTextureFilter::kLinear, false,
                                         pixels.data());
}

void DrawSkullBackground(ImDrawList* draw, rex::ui::ImmediateTexture* texture, float aspect,
                         ImVec2 display, float t, float scale, SkullShade shade_kind) {
  // The wallpaper, covering the screen, in a slow drift and zoom about
  // the skull.
  ImVec2 uv0(0, 0), uv1(1, 1);
  {
    const float screen_aspect = display.x / std::max(display.y, 1.0f);
    float width = 1.0f, height = 1.0f;
    if (screen_aspect > aspect) {
      height = aspect / screen_aspect;
    } else {
      width = screen_aspect / aspect;
    }
    const float zoom = 1.06f + 0.04f * std::sin(t * 0.11f);
    width /= zoom;
    height /= zoom;
    const float cx = std::clamp(0.60f + 0.015f * std::sin(t * 0.07f), width / 2, 1 - width / 2);
    const float cy = std::clamp(0.50f + 0.012f * std::cos(t * 0.09f), height / 2, 1 - height / 2);
    uv0 = ImVec2(cx - width / 2, cy - height / 2);
    uv1 = ImVec2(cx + width / 2, cy + height / 2);
  }
  auto to_screen = [&](float u, float v) {
    return ImVec2((u - uv0.x) / (uv1.x - uv0.x) * display.x,
                  (v - uv0.y) / (uv1.y - uv0.y) * display.y);
  };
  if (texture) {
    draw->AddImage(reinterpret_cast<ImTextureID>(texture), ImVec2(0, 0), display, uv0, uv1);
  } else {
    draw->AddRectFilledMultiColor(ImVec2(0, 0), display, IM_COL32(16, 30, 40, 255),
                                  IM_COL32(16, 30, 40, 255), IM_COL32(4, 8, 12, 255),
                                  IM_COL32(4, 8, 12, 255));
  }
  // The skull's crack glows, pulsing.
  {
    const ImVec2 crack = to_screen(0.77f, 0.24f);
    const float pulse = 0.5f + 0.5f * std::sin(t * 1.8f);
    for (int i = 6; i >= 1; --i) {
      const float radius = (40.0f + i * 34.0f) * scale * (0.9f + 0.15f * pulse);
      draw->AddCircleFilled(crack, radius, IM_COL32(70, 190, 255, int(5 + 5 * pulse)), 48);
    }
  }
  const ImU32 shade = IM_COL32(2, 8, 12, 235), clear = IM_COL32(2, 8, 12, 0);
  if (shade_kind == SkullShade::kLeft) {
    // Dark on the left, under the text; clear over the skull.
    draw->AddRectFilled(ImVec2(0, 0), ImVec2(display.x * 0.38f, display.y), shade);
    draw->AddRectFilledMultiColor(ImVec2(display.x * 0.38f, 0),
                                  ImVec2(display.x * 0.72f, display.y), shade, clear, clear, shade);
  } else {
    // Dark all over, a little less on the skull's side.
    const ImU32 lighter = IM_COL32(2, 8, 12, 175);
    draw->AddRectFilledMultiColor(ImVec2(0, 0), display, shade, lighter, lighter, shade);
  }
  draw->AddRectFilledMultiColor(ImVec2(0, display.y * 0.80f), display, clear, clear,
                                IM_COL32(0, 0, 0, 200), IM_COL32(0, 0, 0, 200));
  // A tactical scan line sweeping down, every 7 seconds.
  {
    const float y = std::fmod(t / 7.0f, 1.0f) * display.y * 1.2f - display.y * 0.1f;
    const float trail = 90.0f * scale;
    draw->AddRectFilledMultiColor(ImVec2(0, y - trail), ImVec2(display.x, y),
                                  IM_COL32(80, 200, 255, 0), IM_COL32(80, 200, 255, 0),
                                  IM_COL32(80, 200, 255, 18), IM_COL32(80, 200, 255, 18));
    draw->AddRectFilled(ImVec2(0, y), ImVec2(display.x, y + 2.0f * scale),
                        IM_COL32(120, 215, 255, 50));
  }
}

bool IsoScreen::HasGameFiles(const std::filesystem::path& folder) {
  std::error_code ec;
  return std::filesystem::exists(folder / kGameFile, ec);
}

IsoScreen::IsoScreen(rex::ui::ImGuiDrawer* imgui_drawer,
                     rex::ui::ImmediateDrawer* immediate_drawer, ImFont* font, ImFont* title_font,
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
  background_ = CreateSkullTexture(immediate_drawer, background_aspect_);
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
  progress_.read_ns = 0;
  progress_.write_ns = 0;
  progress_.cancel = false;
  speed_ = 0.0f;
  speed_sample_at_ = -1.0;
  via_payload_ = false;
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
    // Written where the storage has the folder: through the app's /app0
    // mount, writes slow to a few MB a second.
    const std::filesystem::path target = StorageFolder(game_folder_);
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
      std::filesystem::create_directories(target, ec);
      std::filesystem::space_info space = std::filesystem::space(target, ec);
      if (!ec && space.available < total + (256u << 20)) {
        ok = false;
        error = (portuguese ? "Espaço insuficiente: a extração precisa de "
                            : "Not enough space: the extraction needs ") +
                Gigabytes(total) + (portuguese ? ", livres: " : ", free: ") +
                Gigabytes(space.available) + ".";
      }
    }
    if (ok) {
      bool handled = false;
      ok = ExtractWithPayload(image.path, target, handled, error);
      if (!handled) {
        REXLOG_INFO("Extracting {} into {} in the app", image.path.string(), target.string());
        const auto start = std::chrono::steady_clock::now();
        ok = ExtractDiscImage(
            image.path, files, target, kGameFile, progress_,
            [this](const std::string& path) {
              std::lock_guard<std::mutex> lock(mutex_);
              current_file_ = path;
            },
            error);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        REXLOG_INFO("Extracted {:.2f} GB in {:.0f} s ({:.1f} MB/s)", progress_.done / 1e9,
                    seconds, progress_.done / 1e6 / std::max(seconds, 0.001));
      }
    }
    if (!ok) {
      REXLOG_ERROR("Disc image {}: {}", image.path.string(), error);
      std::lock_guard<std::mutex> lock(mutex_);
      error_ = error;
    }
    state_ = ok ? State::kDone : State::kFailed;
  });
}

bool IsoScreen::ExtractWithPayload(const std::filesystem::path& image,
                                   const std::filesystem::path& target, bool& handled,
                                   std::string& error) {
  handled = false;
  // Only into a folder the payload can see (not the app's own /app0).
  if (target.string().compare(0, 5, "/app0") == 0) {
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(kPayloadFolder, ec);
  const std::filesystem::path folder = kPayloadFolder;
  std::filesystem::remove(folder / "status.txt", ec);
  std::filesystem::remove(folder / "cancel", ec);
  const std::string id = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count() ^ uintptr_t(this));
  {
    std::ofstream out(folder / "request.txt", std::ios::trunc);
    out << id << "\n" << image.string() << "\n" << target.string() << "\n" << kGameFile << "\n";
    if (!out) {
      return false;
    }
  }

  // The payload, to the loader.
  const int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    return false;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(kPayloadPort);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool sent = connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  for (const uint8_t* at = grfs_iso_payload; sent && at < grfs_iso_payload_end;) {
    ssize_t n = send(sock, at, size_t(grfs_iso_payload_end - at), 0);
    sent = n > 0;
    at += sent ? n : 0;
  }
  close(sock);
  if (!sent) {
    REXLOG_INFO("No payload loader on port {}: extracting in the app", kPayloadPort);
    return false;
  }

  // Its status, from the file it rewrites.
  auto read_status = [&](std::string& state, uint64_t& done, uint64_t& total, std::string& line) {
    std::ifstream in(folder / "status.txt");
    std::string status_id;
    if (!(in >> state >> status_id) || status_id != id) {
      return false;
    }
    if (state != "error") {
      in >> done >> total;
    }
    in.ignore(1 << 20, '\n');
    std::getline(in, line);
    return true;
  };
  std::string state, line;
  uint64_t done = 0, total = 0;
  const auto sent_at = std::chrono::steady_clock::now();
  while (!read_status(state, done, total, line)) {
    if (std::chrono::steady_clock::now() - sent_at > std::chrono::seconds(10)) {
      // (if it starts after all, it stops)
      std::ofstream(folder / "cancel");
      REXLOG_WARN("The extractor payload didn't start: extracting in the app");
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  handled = true;
  via_payload_ = true;
  REXLOG_INFO("Extracting {} into {} with the payload", image.string(), target.string());
  const auto start = std::chrono::steady_clock::now();
  bool cancel_sent = false;
  for (;;) {
    if (read_status(state, done, total, line)) {
      progress_.done = done;
      total_ = total;
      if (state == "error") {
        error = line;
        return false;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        current_file_ = line;
      }
      if (state == "done") {
        break;
      }
    }
    if (progress_.cancel && !cancel_sent) {
      std::ofstream(folder / "cancel");
      cancel_sent = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("Extracted {:.2f} GB in {:.0f} s ({:.1f} MB/s) with the payload", done / 1e9,
              seconds, done / 1e6 / std::max(seconds, 0.001));
  return true;
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
  const double now = ImGui::GetTime();
  const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
  if (shown_at_ < 0.0) {
    shown_at_ = now;
  }
  if (state != drawn_state_) {
    drawn_state_ = state;
    state_at_ = now;
  }
  const float t = float(now - shown_at_);
  const float appear = EaseOut(std::clamp(t / 0.8f, 0.0f, 1.0f));
  const float state_appear = EaseOut(std::clamp(float(now - state_at_) / 0.45f, 0.0f, 1.0f));

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                      ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##grfs_iso", nullptr, kFlags)) {
    ImDrawList* draw = ImGui::GetWindowDrawList();

    DrawSkullBackground(draw, background_.get(), background_aspect_, display, t, scale,
                        SkullShade::kLeft);

    if (font_) {
      ImGui::PushFont(font_);
    }
    const float left = display.x * 0.07f;
    const float right = display.x * 0.58f;
    const float top = display.y * 0.10f;
    const float slide = (1.0f - appear) * -80.0f * scale;

    // HUD corner brackets about the panel, drawn out as it appears.
    DrawHudCorners(draw, ImVec2(left - 40.0f * scale, top - 40.0f * scale),
                   ImVec2(right + 40.0f * scale, display.y - 60.0f * scale), scale, appear);

    // The kicker and the title, sliding in.
    const bool extracting = state == State::kExtracting || state == State::kDone;
    ImGui::SetWindowFontScale(font_scale * 0.6f);
    ImGui::SetCursorPos(ImVec2(left + slide, top));
    ImGui::TextColored(WithAlpha(kAccent, appear), "%s", "GHOST RECON: FUTURE SOLDIER  //  PS5");
    if (title_font_) {
      ImGui::PushFont(title_font_);
    }
    ImGui::SetWindowFontScale(font_scale * 0.95f);
    ImGui::SetCursorPos(ImVec2(left + slide * 1.6f, ImGui::GetCursorPosY() + 4.0f * scale));
    ImGui::TextColored(WithAlpha(kText, appear), "%s",
                       extracting ? (portuguese ? "Extraindo os arquivos" : "Extracting the files")
                                  : (portuguese ? "Arquivos do jogo ausentes"
                                                : "Game files missing"));
    if (title_font_) {
      ImGui::PopFont();
    }
    {
      // An accent rule under the title, growing.
      const float y = ImGui::GetCursorPosY() + 6.0f * scale;
      draw->AddRectFilled(ImVec2(left, y), ImVec2(left + 140.0f * scale * appear, y + 4.0f * scale),
                          IM_COL32(90, 200, 255, 255));
      ImGui::SetCursorPosY(y + 34.0f * scale);
    }

    const std::string app = StorageFolder(rex::filesystem::GetExecutableFolder()).string();
    const std::string game_folder = StorageFolder(game_folder_).string();
    const float body_alpha = EaseOut(std::clamp((t - 0.25f) / 0.6f, 0.0f, 1.0f));
    ImGui::SetWindowFontScale(font_scale * 0.72f);
    ImGui::PushTextWrapPos(right);

    // The Discord card, bottom left; the list stops above it.
    const float qr_size = 150.0f * scale;
    const float card_top = display.y - 110.0f * scale - qr_size - 30.0f * scale;
    const float hint_y = display.y - 100.0f * scale;

    if (state == State::kChoosing || state == State::kFailed) {
      ImGui::SetCursorPosX(left);
      ImGui::TextColored(WithAlpha(kDimText, body_alpha), "%s",
                         portuguese ? "Os arquivos extraídos da ISO do jogo (Xbox 360) não foram "
                                      "encontrados. Eles ficam nesta pasta, com o default.xex dentro:"
                                    : "The files extracted from the game's (Xbox 360) ISO weren't "
                                      "found. They go in this folder, with default.xex inside:");
      // The folder, on a dark tag.
      {
        const ImVec2 at(left, ImGui::GetCursorPosY() + 8.0f * scale);
        const ImVec2 size = ImGui::CalcTextSize(game_folder.c_str());
        draw->AddRectFilled(ImVec2(at.x - 2.0f * scale, at.y),
                            ImVec2(at.x + size.x + 28.0f * scale, at.y + size.y + 16.0f * scale),
                            IM_COL32(10, 40, 60, int(220 * body_alpha)), 6.0f * scale);
        draw->AddRectFilled(ImVec2(at.x - 2.0f * scale, at.y),
                            ImVec2(at.x + 4.0f * scale, at.y + size.y + 16.0f * scale),
                            IM_COL32(90, 200, 255, int(255 * body_alpha)));
        ImGui::SetCursorPos(ImVec2(at.x + 16.0f * scale, at.y + 8.0f * scale));
        ImGui::TextColored(WithAlpha(kAccent, body_alpha), "%s", game_folder.c_str());
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 26.0f * scale);
      }
      ImGui::SetCursorPosX(left);
      ImGui::TextColored(WithAlpha(kDimText, body_alpha), "%s",
                         (portuguese ? "Ou coloque o .iso do jogo em " + app +
                                           " ou na raiz de um pendrive ou HD externo, escolha-o "
                                           "abaixo e aperte X: os arquivos são extraídos e o "
                                           "jogo inicia."
                                     : "Or put the game's .iso in " + app +
                                           " or at the root of a USB drive, choose it below and "
                                           "press X: its files are extracted and the game starts.")
                             .c_str());
    }
    ImGui::PopTextWrapPos();

    const float list_top = ImGui::GetCursorPosY() + 34.0f * scale;
    if (state == State::kChoosing) {
      const float list_alpha = EaseOut(std::clamp((t - 0.45f) / 0.6f, 0.0f, 1.0f));
      ImGui::SetWindowFontScale(font_scale * 0.6f);
      ImGui::SetCursorPos(ImVec2(left, list_top));
      ImGui::TextColored(WithAlpha(kAccent, list_alpha), "%s",
                         portuguese ? "ISOS ENCONTRADAS" : "ISOS FOUND");
      ImGui::SetWindowFontScale(font_scale * 0.72f);
      const float rows_top = ImGui::GetCursorPosY() + 10.0f * scale;
      const float row_height = 70.0f * scale;
      if (images_.empty()) {
        ImGui::SetCursorPos(ImVec2(left, rows_top));
        ImGui::PushTextWrapPos(right);
        ImGui::TextColored(WithAlpha(kDimText, list_alpha * (0.65f + 0.35f * std::sin(t * 2.5f))),
                           "%s",
                           portuguese ? "Nenhuma ISO encontrada. Coloque uma e aperte Quadrado "
                                        "para procurar de novo."
                                      : "No ISO found. Put one there and press Square to search "
                                        "again.");
        ImGui::PopTextWrapPos();
      }
      const int rows = std::max(1, int((card_top - rows_top - 20.0f * scale) / row_height));
      const int first =
          std::clamp(selected_ - rows + 1, 0, std::max(int(images_.size()) - rows, 0));
      // The highlight slides to the selected row.
      highlight_row_ += (float(selected_ - first) - highlight_row_) * std::min(1.0f, dt * 14.0f);
      if (!images_.empty()) {
        const float y = rows_top + highlight_row_ * row_height;
        const float pulse = 0.5f + 0.5f * std::sin(t * 3.0f);
        draw->AddRectFilledMultiColor(ImVec2(left, y), ImVec2(right, y + row_height - 10.0f * scale),
                                      IM_COL32(40, 140, 210, int(200 * list_alpha)),
                                      IM_COL32(40, 140, 210, int(40 * list_alpha)),
                                      IM_COL32(40, 140, 210, int(40 * list_alpha)),
                                      IM_COL32(40, 140, 210, int(200 * list_alpha)));
        draw->AddRect(ImVec2(left, y), ImVec2(right, y + row_height - 10.0f * scale),
                      IM_COL32(110, 210, 255, int((90 + 110 * pulse) * list_alpha)), 4.0f * scale,
                      0, 2.0f * scale);
        draw->AddRectFilled(ImVec2(left, y), ImVec2(left + 6.0f * scale, y + row_height - 10.0f * scale),
                            IM_COL32(120, 220, 255, int(255 * list_alpha)));
      }
      for (int i = first; i < int(images_.size()) && i < first + rows; ++i) {
        const float y = rows_top + (i - first) * row_height;
        const float row_alpha =
            list_alpha * EaseOut(std::clamp((t - 0.55f - 0.08f * (i - first)) / 0.4f, 0.0f, 1.0f));
        const bool chosen = i == selected_;
        if (!chosen) {
          draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + row_height - 10.0f * scale),
                              IM_COL32(255, 255, 255, int(14 * row_alpha)), 4.0f * scale);
        }
        const float text_y = y + (row_height - 10.0f * scale - ImGui::GetTextLineHeight()) / 2;
        ImGui::SetCursorPos(ImVec2(left + 26.0f * scale + (1.0f - row_alpha) * 30.0f * scale, text_y));
        ImGui::TextColored(WithAlpha(kText, row_alpha), "%s",
                           images_[i].path.filename().string().c_str());
        const std::string detail =
            StorageFolder(images_[i].path.parent_path()).string() + "   " + Gigabytes(images_[i].size);
        const float detail_width = ImGui::CalcTextSize(detail.c_str()).x;
        ImGui::SetCursorPos(ImVec2(right - 22.0f * scale - detail_width, text_y));
        ImGui::TextColored(WithAlpha(chosen ? kText : kDimText, row_alpha), "%s", detail.c_str());
      }
    } else if (extracting) {
      const uint64_t total = total_, done = std::min<uint64_t>(progress_.done, total);
      const float fraction = state == State::kDone ? 1.0f
                             : total ? float(double(done) / double(total))
                                     : 0.0f;
      shown_progress_ += (fraction - shown_progress_) * std::min(1.0f, dt * 5.0f);
      std::string file;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        file = current_file_;
      }
      ImGui::SetWindowFontScale(font_scale * 0.72f);
      ImGui::SetCursorPos(ImVec2(left, ImGui::GetCursorPosY()));
      ImGui::PushTextWrapPos(right);
      ImGui::TextColored(WithAlpha(kDimText, state_appear), "%s",
                         ((portuguese ? "Para a pasta " : "Into ") + game_folder).c_str());
      ImGui::SetCursorPosX(left);
      ImGui::TextColored(WithAlpha(kDimText, state_appear), "%s",
                         portuguese ? "Não feche o jogo nem desligue o console. O jogo inicia "
                                      "quando terminar."
                                    : "Don't close the game or turn the console off. The game "
                                      "starts when it's done.");
      ImGui::PopTextWrapPos();

      // The percentage, large, with a spinner.
      const float big_y = ImGui::GetCursorPosY() + 40.0f * scale;
      char percent[16];
      std::snprintf(percent, sizeof(percent), "%d%%", int(shown_progress_ * 100.0f + 0.5f));
      if (title_font_) {
        ImGui::PushFont(title_font_);
      }
      ImGui::SetWindowFontScale(font_scale * 1.3f);
      ImGui::SetCursorPos(ImVec2(left, big_y));
      ImGui::TextColored(WithAlpha(kText, state_appear), "%s", percent);
      const float big_height = ImGui::GetTextLineHeight();
      if (title_font_) {
        ImGui::PopFont();
      }
      {
        const ImVec2 c(right - 34.0f * scale, big_y + big_height / 2);
        const float r = 26.0f * scale, a = float(now * 5.0);
        draw->PathArcTo(c, r, a, a + 4.2f, 32);
        draw->PathStroke(IM_COL32(90, 200, 255, int(255 * state_appear)), 0, 5.0f * scale);
        draw->AddCircle(c, r, IM_COL32(90, 200, 255, int(50 * state_appear)), 32, 5.0f * scale);
      }

      // The bar: moving stripes in its fill, and a shine running along.
      const float bar_y = big_y + big_height + 20.0f * scale;
      const float bar_height = 22.0f * scale;
      const ImVec2 bar_a(left, bar_y), bar_b(right, bar_y + bar_height);
      draw->AddRectFilled(bar_a, bar_b, IM_COL32(255, 255, 255, 28), bar_height / 2);
      const float fill_x = left + (right - left) * std::clamp(shown_progress_, 0.0f, 1.0f);
      if (fill_x > left + 1.0f) {
        draw->AddRectFilledMultiColor(bar_a, ImVec2(fill_x, bar_b.y), IM_COL32(30, 120, 200, 255),
                                      IM_COL32(90, 210, 255, 255), IM_COL32(90, 210, 255, 255),
                                      IM_COL32(30, 120, 200, 255));
        draw->PushClipRect(bar_a, ImVec2(fill_x, bar_b.y), true);
        const float stripe = 26.0f * scale, offset = std::fmod(float(now) * 50.0f * scale, stripe * 2);
        for (float x = left - bar_height - stripe * 2 + offset; x < fill_x; x += stripe * 2) {
          draw->AddQuadFilled(ImVec2(x, bar_b.y), ImVec2(x + stripe, bar_b.y),
                              ImVec2(x + stripe + bar_height, bar_a.y), ImVec2(x + bar_height, bar_a.y),
                              IM_COL32(255, 255, 255, 30));
        }
        const float shine = left + std::fmod(float(now) * 0.6f, 1.4f) * (right - left) - 120.0f * scale;
        draw->AddRectFilledMultiColor(ImVec2(shine, bar_a.y), ImVec2(shine + 60.0f * scale, bar_b.y),
                                      IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 90),
                                      IM_COL32(255, 255, 255, 90), IM_COL32(255, 255, 255, 0));
        draw->AddRectFilledMultiColor(ImVec2(shine + 60.0f * scale, bar_a.y),
                                      ImVec2(shine + 120.0f * scale, bar_b.y),
                                      IM_COL32(255, 255, 255, 90), IM_COL32(255, 255, 255, 0),
                                      IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 90));
        draw->PopClipRect();
      }
      ImGui::SetWindowFontScale(font_scale * 0.65f);
      ImGui::SetCursorPos(ImVec2(left, bar_b.y + 18.0f * scale));
      // The speed, over the last second.
      if (speed_sample_at_ < 0.0 || now - speed_sample_at_ >= 1.0) {
        if (speed_sample_at_ >= 0.0) {
          speed_ = float(double(done - speed_sample_done_) / (now - speed_sample_at_) / 1e6);
        }
        speed_sample_at_ = now;
        speed_sample_done_ = done;
      }
      char speed[160];
      std::snprintf(speed, sizeof(speed), "%s / %s   %.0f MB/s", Gigabytes(done).c_str(),
                    Gigabytes(total).c_str(), speed_);
      ImGui::TextColored(WithAlpha(kText, state_appear), "%s", speed);
      ImGui::SetCursorPosX(left);
      ImGui::PushTextWrapPos(right);
      ImGui::TextColored(WithAlpha(kDimText, state_appear), "%s", file.c_str());
      // How it's extracting: the payload at full speed, or the app, slowed.
      ImGui::SetCursorPos(ImVec2(left, ImGui::GetCursorPosY() + 16.0f * scale));
      if (via_payload_) {
        ImGui::TextColored(WithAlpha(kAccent, state_appear), "%s",
                           portuguese ? "Extração rápida pelo carregador de payloads (porta 9021)."
                                      : "Fast extraction through the payload loader (port 9021).");
      } else if (state == State::kExtracting && progress_.done > 0) {
        ImGui::TextColored(WithAlpha(kDimText, state_appear), "%s",
                           portuguese ? "Sem carregador de payloads, o PS5 deixa o jogo gravar "
                                        "devagar depois dos primeiros GB. Com o etaHEN ou o "
                                        "elfldr ativo (porta 9021), a extração fica muito mais "
                                        "rápida: cancele, ative um deles e tente de novo."
                                      : "Without a payload loader, the PS5 slows the game's "
                                        "writes after its first GBs. With etaHEN or elfldr "
                                        "running (port 9021) it's much faster: cancel, start "
                                        "one of them and try again.");
      }
      ImGui::PopTextWrapPos();
    }
    if (state == State::kFailed) {
      std::string error;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        error = error_;
      }
      // A red panel, shaking in.
      const float shake = (1.0f - state_appear) * std::sin(float(now - state_at_) * 60.0f) * 14.0f * scale;
      ImGui::SetWindowFontScale(font_scale * 0.72f);
      const std::string message =
          (portuguese ? "Não foi possível extrair a ISO: " : "Couldn't extract the ISO: ") + error;
      const ImVec2 size = ImGui::CalcTextSize(message.c_str(), nullptr, false, right - left - 50.0f * scale);
      const ImVec2 a(left + shake, list_top), b(right + shake, list_top + size.y + 40.0f * scale);
      draw->AddRectFilled(a, b, IM_COL32(90, 20, 20, int(220 * state_appear)), 6.0f * scale);
      draw->AddRectFilled(a, ImVec2(a.x + 6.0f * scale, b.y), IM_COL32(255, 90, 80, int(255 * state_appear)));
      ImGui::SetCursorPos(ImVec2(a.x + 28.0f * scale, a.y + 20.0f * scale));
      ImGui::PushTextWrapPos(right + shake - 22.0f * scale);
      ImGui::TextColored(WithAlpha(kError, state_appear), "%s", message.c_str());
      ImGui::PopTextWrapPos();
    }

    // Discord, bottom left.
    {
      const float card_alpha = EaseOut(std::clamp((t - 0.7f) / 0.6f, 0.0f, 1.0f));
      const ImVec2 at(left + (1.0f - card_alpha) * -40.0f * scale, card_top);
      DrawQr(draw, at, qr_size, scale, kDiscordUrl, card_alpha);
      ImGui::SetWindowFontScale(font_scale * 0.6f);
      ImGui::SetCursorPos(ImVec2(at.x + qr_size + 28.0f * scale, at.y + 18.0f * scale));
      ImGui::TextColored(WithAlpha(kAccent, card_alpha), "%s", "DISCORD");
      ImGui::SetWindowFontScale(font_scale * 0.7f);
      ImGui::SetCursorPosX(at.x + qr_size + 28.0f * scale);
      ImGui::TextColored(WithAlpha(kDimText, card_alpha), "%s",
                         portuguese ? "Dúvidas ou problemas? Fale com a gente:"
                                    : "Questions or problems? Talk to us:");
      ImGui::SetCursorPosX(at.x + qr_size + 28.0f * scale);
      ImGui::TextColored(WithAlpha(kText, card_alpha), "%s", kDiscordLink);
    }

    // The buttons.
    ImGui::SetWindowFontScale(font_scale * 0.75f);
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

    // Fading in from black.
    if (appear < 1.0f) {
      ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), display,
                                                    IM_COL32(0, 0, 0, int(255 * (1.0f - appear))));
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}
