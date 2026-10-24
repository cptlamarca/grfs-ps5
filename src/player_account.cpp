// See player_account.h. The hello (POST /player on the news server, next to
// news_url's file) and its answer are "key=value" lines: serial, name and xp
// (the six classes' XP), answered with banned, reason and xp, the XP the
// game uses from then on (the server's own, at least the game's).

#include "player_account.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
#include <sys/sysctl.h>
// (weak, checked before the calls: the hardware serial number's function is
// only in libkernel_sys, which games don't get, its import resolving to 0)
extern "C" int sceKernelGetOpenPsId(uint8_t id[16]) __attribute__((weak));
extern "C" int sceKernelGetOpenPsIdForSystem(uint8_t id[16]) __attribute__((weak));
#pragma weak sysctlbyname
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

// The decision points' choices (src/ai_hooks.cpp): "ID choice" lines.
std::filesystem::path ChoicesPath() {
  return rex::system::kernel_state()->content_manager()->ResolveGameUserContentPath() /
         "system_link_choices.txt";
}

std::map<uint32_t, uint32_t> ReadChoices() {
  std::map<uint32_t, uint32_t> choices;
  std::ifstream file(ChoicesPath());
  for (uint32_t id, choice; file >> id >> choice;) {
    if (choice == 1 || choice == 2) {
      choices.emplace(id, choice);
    }
  }
  return choices;
}

void WriteChoices(const std::map<uint32_t, uint32_t>& choices) {
  std::filesystem::path path = ChoicesPath();
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::trunc);
    for (const auto& [id, choice] : choices) {
      file << id << ' ' << choice << '\n';
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
}

// The attachments bought with credits (src/ai_hooks.cpp): "spent CLASS N"
// and "weapon CLASS NUMBER BOUGHT" (hex) lines.
std::filesystem::path AttachmentsPath() {
  return rex::system::kernel_state()->content_manager()->ResolveGameUserContentPath() /
         "system_link_attachments.txt";
}

struct Attachments {
  std::array<uint32_t, 3> spent{};
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> bought;
  bool operator==(const Attachments&) const = default;
};

Attachments ReadAttachments() {
  Attachments attachments;
  std::ifstream file(AttachmentsPath());
  for (std::string kind; file >> kind;) {
    uint32_t row = 0, number = 0;
    if (kind == "spent" && file >> row >> number && row < 3) {
      attachments.spent[row] = number;
    } else if (kind == "weapon" && file >> row >> number && row < 3) {
      uint64_t bought = 0;
      file >> std::hex >> bought >> std::dec;
      attachments.bought[{row, number}] = bought;
    } else {
      file.ignore(1 << 20, '\n');
    }
  }
  return attachments;
}

void WriteAttachments(const Attachments& attachments) {
  std::filesystem::path path = AttachmentsPath();
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::trunc);
    for (uint32_t row = 0; row < 3; ++row) {
      file << "spent " << row << ' ' << attachments.spent[row] << '\n';
    }
    for (const auto& [key, bought] : attachments.bought) {
      file << "weapon " << key.first << ' ' << key.second << ' ' << std::hex << bought << std::dec
           << '\n';
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
}

// The player's data (UserData: the campaign's saves, the profile and its
// System Link files, the trophies; not the cache), as one block for the news
// server's /save: "GRSV1\n", then per file its path (relative, '/'), a line
// with its size, and its bytes.
constexpr char kSaveMagic[] = "GRSV1\n";

std::vector<std::filesystem::path> SaveFiles(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> files;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(root, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    const std::filesystem::path relative = it->path().lexically_relative(root);
    if (it->is_directory(error) && relative == "cache") {
      it.disable_recursion_pending();
    } else if (it->is_regular_file(error) && it->path().extension() != ".tmp") {
      files.push_back(relative);
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

// What the files are now (paths, sizes and times), to notice changes.
std::string SaveState(const std::filesystem::path& root) {
  std::ostringstream state;
  std::error_code error;
  for (const auto& file : SaveFiles(root)) {
    state << file.generic_string() << ' ' << std::filesystem::file_size(root / file, error) << ' '
          << int64_t(std::filesystem::last_write_time(root / file, error).time_since_epoch().count())
          << '\n';
  }
  return state.str();
}

std::string PackSave(const std::filesystem::path& root) {
  std::string block = kSaveMagic;
  for (const auto& file : SaveFiles(root)) {
    std::ifstream in(root / file, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    block += file.generic_string() + "\n" + std::to_string(data.size()) + "\n" + data;
  }
  return block;
}

// Writes a block's files under root, any path leaving it refused.
bool UnpackSave(const std::string& block, const std::filesystem::path& root) {
  if (block.rfind(kSaveMagic, 0) != 0) {
    return false;
  }
  size_t at = sizeof(kSaveMagic) - 1;
  while (at < block.size()) {
    size_t name_end = block.find('\n', at);
    size_t size_end = name_end == std::string::npos ? name_end : block.find('\n', name_end + 1);
    if (size_end == std::string::npos) {
      return false;
    }
    const std::filesystem::path relative =
        std::filesystem::path(block.substr(at, name_end - at)).lexically_normal();
    const size_t size = std::strtoull(block.c_str() + name_end + 1, nullptr, 10);
    at = size_end + 1;
    if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
        at + size > block.size()) {
      return false;
    }
    std::error_code error;
    std::filesystem::create_directories((root / relative).parent_path(), error);
    std::ofstream(root / relative, std::ios::binary | std::ios::trunc)
        .write(block.data() + at, std::streamsize(size));
    at += size;
  }
  return true;
}

// Whether there's a campaign save (a .sav file) under root.
bool HasCampaignSave(const std::filesystem::path& root) {
  for (const auto& file : SaveFiles(root)) {
    if (file.extension() == ".sav") {
      return true;
    }
  }
  return false;
}

// The PS5's console ID (its OpenPSID, unique and fixed, as hex; empty
// elsewhere), which the server knows the console by.
std::string ReadSerialNumber() {
#if REX_PLATFORM_PS5
  static constexpr char kHex[] = "0123456789ABCDEF";
  uint8_t id[16] = {};
  auto hex = [&id] {
    std::string text;
    for (uint8_t byte : id) {
      text += kHex[byte >> 4];
      text += kHex[byte & 15];
    }
    return text.find_first_not_of('0') != std::string::npos ? text : std::string();
  };
  // Its sources, the first that answers: games get the sysctl (the
  // functions answer them with errors, but are kept in case).
  size_t size = sizeof(id);
  if (sysctlbyname && sysctlbyname("machdep.openpsid", id, &size, nullptr, 0) == 0 &&
      size == sizeof(id) && !hex().empty()) {
    return hex();
  }
  for (auto* get : {sceKernelGetOpenPsId, sceKernelGetOpenPsIdForSystem}) {
    std::memset(id, 0, sizeof(id));
    if (get && get(id) == 0 && !hex().empty()) {
      return hex();
    }
  }
  REXKRNL_WARN("Player account: no console ID");
#endif
  return {};
}

// One hello: the server's XP replaces the file's when they differ; the
// server's choices are added to the console's (or replace them, after the
// page reset them).
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
  const std::map<uint32_t, uint32_t> choices = ReadChoices();
  request << "choices=";
  bool first = true;
  for (const auto& [id, choice] : choices) {
    request << (first ? "" : ",") << id << ':' << choice;
    first = false;
  }
  request << "\n";
  const Attachments attachments = ReadAttachments();
  request << "spent=" << attachments.spent[0] << ',' << attachments.spent[1] << ','
          << attachments.spent[2] << "\nbought=";
  first = true;
  for (const auto& [key, bought] : attachments.bought) {
    request << (first ? "" : ",") << key.first << ':' << key.second << ':' << std::hex << bought
            << std::dec;
    first = false;
  }
  request << "\n";

  std::string body;
  if (!HttpPost(url, request.str(), body)) {
    return;
  }
  bool now_banned = false;
  std::string reason;
  Totals server = totals;
  bool have_server_xp = false, have_server_choices = false, replace = false;
  std::map<uint32_t, uint32_t> server_choices;
  Attachments server_attachments;
  bool have_server_attachments = false;
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
    } else if (key == "choices") {
      have_server_choices = true;
      std::istringstream pairs(value);
      for (std::string pair; std::getline(pairs, pair, ',');) {
        size_t colon = pair.find(':');
        if (colon != std::string::npos) {
          uint32_t choice = uint32_t(std::strtoul(pair.c_str() + colon + 1, nullptr, 10));
          if (choice == 1 || choice == 2) {
            server_choices.emplace(uint32_t(std::strtoul(pair.c_str(), nullptr, 10)), choice);
          }
        }
      }
    } else if (key == "spent") {
      have_server_attachments = true;
      std::istringstream numbers(value);
      size_t i = 0;
      for (std::string number; i < 3 && std::getline(numbers, number, ','); ++i) {
        server_attachments.spent[i] = uint32_t(std::strtoul(number.c_str(), nullptr, 10));
      }
    } else if (key == "bought") {
      std::istringstream items(value);
      for (std::string item; std::getline(items, item, ',');) {
        uint32_t row = 0, number = 0;
        char hex[17] = {};
        if (std::sscanf(item.c_str(), "%u:%u:%16[0-9a-fA-F]", &row, &number, hex) == 3 && row < 3) {
          server_attachments.bought[{row, number}] = std::strtoull(hex, nullptr, 16);
        }
      }
    } else if (key == "replace") {
      replace = value == "1";
    }
  }
  if (have_server_attachments) {
    Attachments merged = replace ? server_attachments : attachments;
    if (!replace) {
      for (size_t i = 0; i < 3; ++i) {
        merged.spent[i] = std::max(merged.spent[i], server_attachments.spent[i]);
      }
      for (const auto& [key, bought] : server_attachments.bought) {
        merged.bought[key] |= bought;
      }
    }
    if (!(merged == attachments)) {
      REXKRNL_INFO("Player account: attachments bought from the server");
      WriteAttachments(merged);
    }
  }
  if (have_server_choices) {
    std::map<uint32_t, uint32_t> merged = replace ? server_choices : choices;
    if (!replace) {
      merged.insert(server_choices.begin(), server_choices.end());
    }
    if (merged != choices) {
      REXKRNL_INFO("Player account: decision point choices from the server");
      WriteChoices(merged);
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
  // A console without a campaign save (the game installed again) gets its
  // backup back before the game starts.
  const std::filesystem::path user_data = runtime->user_data_root();
  std::string news = rex::cvar::Query<std::string>("news_url");
  size_t news_slash = news.rfind('/');
  const std::string news_base =
      news_slash != std::string::npos && news_slash > 7 ? news.substr(0, news_slash) : "";
  if (!news_base.empty() && !HasCampaignSave(user_data)) {
    std::string block;
    if (HttpGet(news_base + "/save?serial=" + serial, block) && UnpackSave(block, user_data)) {
      REXKRNL_INFO("Player account: the saves came back from the server ({} bytes)",
                   block.size());
    }
  }
  std::thread([serial, runtime, user_data, news_base] {
    // Once the game has its profile, then every half minute (the XP file
    // changes after matches).
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::filesystem::file_time_type synced{};
    std::string saved_state;
    for (int round = 0;; ++round) {
      std::string news = rex::cvar::Query<std::string>("news_url");
      size_t slash = news.rfind('/');
      if (slash != std::string::npos && slash > 7) {
        std::error_code error;
        std::error_code choices_error;
        auto changed = std::max({std::filesystem::last_write_time(XpPath(), error),
                                 std::filesystem::last_write_time(ChoicesPath(), choices_error),
                                 std::filesystem::last_write_time(AttachmentsPath(),
                                                                  choices_error)});
        // (every 5 minutes anyway, for bans)
        if (round % 10 == 0 || error || changed != synced) {
          // (the account of the pad of player 1, the PS5's user)
          std::string name;
          if (auto* input = static_cast<rex::input::InputSystem*>(runtime->input_system())) {
            name = input->GetUserName(0);
          }
          Sync(news.substr(0, slash) + "/player", serial, name);
          synced = std::max({std::filesystem::last_write_time(XpPath(), error),
                             std::filesystem::last_write_time(ChoicesPath(), choices_error),
                             std::filesystem::last_write_time(AttachmentsPath(), choices_error)});
        }
      }
      // The player's data, at most every 10 minutes when it changed.
      if (!news_base.empty() && round % 20 == 0) {
        std::string state = SaveState(user_data);
        if (state != saved_state && HasCampaignSave(user_data)) {
          std::string answer;
          if (HttpPost(news_base + "/save?serial=" + serial, PackSave(user_data), answer)) {
            saved_state = state;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::seconds(30));
    }
  }).detach();
}

std::string ConsoleId() {
  std::lock_guard<std::mutex> lock(account_mutex);
  return serial_number;
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
