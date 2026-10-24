// See player_account.h. The hello (POST /player on the news server, next to
// news_url's file) and its answer are "key=value" lines: serial, name and xp
// (the six classes' XP), answered with banned, reason and xp, the XP the
// game uses from then on (the server's own, at least the game's).

#include "player_account.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/system_link_relay.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

#include "news_hooks.h"

#if REX_PLATFORM_PS5
extern "C" int sceKernelGetHwSerialNumber(char* serial);
#endif

namespace {

// The XP file of src/ai_hooks.cpp: "GRX2", the six classes' XP (u32), the
// hash of the match result last counted (u64).
constexpr char kXpMagic[4] = {'G', 'R', 'X', '2'};
constexpr size_t kClassCount = 6;
using Totals = std::array<uint32_t, kClassCount>;

std::mutex account_mutex;
std::string serial_number;
bool banned = false;
std::string ban_reason;

std::filesystem::path XpPath() {
  return rex::system::kernel_state()->content_manager()->ResolveGameUserContentPath() /
         "system_link_xp.bin";
}

bool ReadXp(Totals& totals, uint64_t& counted) {
  std::ifstream file(XpPath(), std::ios::binary);
  char magic[sizeof(kXpMagic)];
  return file.read(magic, sizeof(magic)) && !std::memcmp(magic, kXpMagic, sizeof(magic)) &&
         file.read(reinterpret_cast<char*>(totals.data()), sizeof(totals)) &&
         file.read(reinterpret_cast<char*>(&counted), sizeof(counted));
}

void WriteXp(const Totals& totals, uint64_t counted) {
  std::filesystem::path path = XpPath();
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(kXpMagic, sizeof(kXpMagic));
    file.write(reinterpret_cast<const char*>(totals.data()), sizeof(totals));
    file.write(reinterpret_cast<const char*>(&counted), sizeof(counted));
  }
  std::filesystem::rename(temporary, path, error);
}

// The PS5's serial number (empty elsewhere).
std::string ReadSerialNumber() {
#if REX_PLATFORM_PS5
  char serial[64] = {};
  if (sceKernelGetHwSerialNumber(serial) == 0) {
    std::string text;
    for (char c : serial) {
      if (std::isalnum(static_cast<unsigned char>(c))) {
        text += c;
      } else if (!c) {
        break;
      }
    }
    return text;
  }
#endif
  return {};
}

// One hello: the server's XP replaces the file's when they differ.
void Sync(const std::string& url, const std::string& serial, const std::string& name) {
  Totals totals{};
  uint64_t counted = 0;
  bool have_xp = ReadXp(totals, counted);

  std::ostringstream request;
  request << "serial=" << serial << "\n";
  request << "name=" << name << "\n";
  if (have_xp) {
    request << "xp=";
    for (size_t i = 0; i < kClassCount; ++i) {
      request << (i ? "," : "") << totals[i];
    }
    request << "\n";
  }

  std::string body;
  if (!HttpPost(url, request.str(), body)) {
    return;
  }
  bool now_banned = false;
  std::string reason;
  Totals server = totals;
  bool have_server_xp = false;
  std::istringstream lines(body);
  for (std::string line; std::getline(lines, line);) {
    size_t equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    std::string key = line.substr(0, equals), value = line.substr(equals + 1);
    if (key == "banned") {
      now_banned = value == "1";
    } else if (key == "reason") {
      reason = value;
    } else if (key == "xp") {
      std::istringstream numbers(value);
      size_t i = 0;
      for (std::string number; i < kClassCount && std::getline(numbers, number, ','); ++i) {
        server[i] = uint32_t(std::strtoul(number.c_str(), nullptr, 10));
      }
      have_server_xp = i == kClassCount;
    }
  }
  if (have_server_xp && server != totals) {
    REXKRNL_INFO("Player account: the server's XP replaces this console's");
    WriteXp(server, counted);
  }
  std::lock_guard<std::mutex> lock(account_mutex);
  if (now_banned != banned) {
    REXKRNL_INFO("Player account: {}", now_banned ? "banned from the online" : "not banned");
  }
  banned = now_banned;
  ban_reason = reason;
}

}  // namespace

void StartPlayerAccount(rex::Runtime* runtime) {
  std::string serial = ReadSerialNumber();
  if (serial.empty()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(account_mutex);
    serial_number = serial;
  }
  REXKRNL_INFO("Player account: console {}", serial);
  rex::kernel::xam::system_link_relay::SetIdentity(serial);
  std::thread([serial, runtime] {
    // Once the game has its profile, then every half minute (the XP file
    // changes after matches).
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::filesystem::file_time_type synced{};
    for (int round = 0;; ++round) {
      std::string news = rex::cvar::Query<std::string>("news_url");
      size_t slash = news.rfind('/');
      if (slash != std::string::npos && slash > 7) {
        std::error_code error;
        auto changed = std::filesystem::last_write_time(XpPath(), error);
        // (every 5 minutes anyway, for bans)
        if (round % 10 == 0 || error || changed != synced) {
          // (the account of the pad of player 1, the PS5's user)
          std::string name;
          if (auto* input = static_cast<rex::input::InputSystem*>(runtime->input_system())) {
            name = input->GetUserName(0);
          }
          Sync(news.substr(0, slash) + "/player", serial, name);
          synced = std::filesystem::last_write_time(XpPath(), error);
        }
      }
      std::this_thread::sleep_for(std::chrono::seconds(30));
    }
  }).detach();
}

bool PlayerBanned(std::string* reason) {
  std::string relay_reason;
  if (rex::kernel::xam::system_link_relay::Denied(&relay_reason)) {
    if (reason) {
      *reason = relay_reason;
    }
    return true;
  }
  std::lock_guard<std::mutex> lock(account_mutex);
  if (reason) {
    *reason = ban_reason;
  }
  return banned;
}
