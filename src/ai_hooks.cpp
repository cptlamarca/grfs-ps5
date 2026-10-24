// Patches to the AI module (AICLASS_X360_F.xex, where the game logic lives),
// built into that module so they override its weak recompiled functions.

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

REXCVAR_DEFINE_BOOL(system_link_progression, true, "GRFS",
                    "Class levels and item unlocks in System Link, as online: without it, "
                    "System Link games have every item unlocked");

namespace {

// The player's progression: six classes (two factions of three) of 120 bytes
// from +51288, with their XP at +104 and rank at +111. Online, both come from
// the servers' stats. In System Link, the game puts the stats of a match that
// ends in its career variable, which loading the progression reads into the
// classes; resetting the progression clears them. So the XP of each match
// result is added up in a file of the profile, and the classes get their
// totals back, with the ranks they reach.
constexpr uint32_t kClasses = 51288, kClassSize = 120, kClassXp = 104, kClassRank = 111;
// The rank of the class's last decision point chosen, and a changed mark.
constexpr uint32_t kClassDecision = 112, kClassChanged = 118;
constexpr uint32_t kClassCount = 6;
// The game's settings: the number of ranks (u8 at +4690) and the XP each one
// takes from the start (u32 table at *(+4696)).
constexpr uint32_t kSettingsPointer = 0x887FDF58, kRankCount = 4690, kRankXpPointer = 4696;
// The game executable's variables: a table (at *kVariables) of u16 offsets
// into its data, one per variable at 2 * (id + 27648), 0xFFFF for variables
// that don't exist. The career is variable 12, its size variable 13.
constexpr uint32_t kVariables = 0x82EDE1A4, kCareer = 12, kCareerSize = 13;
constexpr char kXpFileName[] = "system_link_xp.bin";
// Decision points: at some ranks a class gets one of two choices of items.
// Online, the choice was made when the rank was reached and kept by the
// servers. In System Link the player makes it on a screen of the port
// (src/support_screen.cpp), from the first one waiting, written to the
// profile's kDecisionFileName ("ID\tclass\trank\tTITLES\tTITLES\tTEXT\tTEXT\tITEM\tITEM",
// the titles' text IDs of each choice separated by commas, then the text ID
// of each choice's description, then its first item's "type:number", for its
// icon); the choices go to
// kChoicesFileName ("ID choice" lines, 1 or 2), and their items into the
// class's unlocked items whenever the progression is worked out.
constexpr char kDecisionFileName[] = "system_link_decision.txt";
constexpr char kChoicesFileName[] = "system_link_choices.txt";
// The game's unlockables (st_Unlockable, 104 bytes): the service table's
// functions +16 and +24 give their count and each one from a handle. A
// decision point has +8 set (a byte), its rank at +12 and its class at +16
// (23 the Scout, 26 the Engineer, 20 the Rifleman); then four items of 20
// bytes from +24: type, number, title's and description's text IDs. The items of the first
// choice are the first two (a weapon and the other faction's), the second's
// the last two.
constexpr uint32_t kServices = 0x887E9E0C, kUnlockables = 0x887FDF84;
constexpr uint32_t kServiceCount = 16, kServiceGet = 24;
// The player's progression, as the menus' commands find it.
constexpr uint32_t kProgression = 0x887FDF38;
// The classes' unlocked items: six of 104 bytes from +52016 (rows 0-2 and
// the other faction's 3-5). Attachments: a u64 of those still locked for
// every weapon (+48, bit = attachment number), and the weapons (+36 count, at
// *(+40), 16 bytes: a u64 of the attachments bought for that one, then its
// number at +8).
constexpr uint32_t kInventories = 52016, kInventorySize = 104;
constexpr uint32_t kInventoryWeaponCount = 36, kInventoryWeapons = 40, kInventoryLocked = 48;
constexpr uint32_t kWeaponSize = 16, kWeaponNumber = 8;
// Attachment credits: a class gets one per rank it gains (+88 those left,
// +90 those earned), as the game adds them at the end of a match
// (sub_8818C6D0); each buys an attachment for one weapon. The ranks come
// from the totals here, so the credits are its rank less those spent, and the
// attachments bought (which a new progression locks again) are kept in the
// profile's kAttachmentsFileName ("spent CLASS N" and "weapon CLASS NUMBER
// LOCKED" lines, LOCKED the weapon's locked attachments in hex).
constexpr uint32_t kClassCredits = 88, kClassCreditsEarned = 90;
constexpr char kAttachmentsFileName[] = "system_link_attachments.txt";
constexpr char kXpMagic[4] = {'G', 'R', 'X', '2'};

struct XpFile {
  std::array<uint32_t, kClassCount> totals{};
  // The match result last added up (a hash of the career variable).
  uint64_t counted_career = 0;
};

std::mutex xp_mutex;
bool xp_loaded = false;
XpFile xp;
// The file's time when last read or written: the player account
// (src/player_account.cpp) replaces the file with the server's XP.
std::filesystem::file_time_type xp_time;

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

void StoreGuest32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

// The guest address of a game variable, 0 if it doesn't exist.
uint32_t VariableAddress(const uint8_t* base, uint32_t id) {
  uint32_t table = LoadGuest32(base, kVariables);
  if (!table) {
    return 0;
  }
  const uint8_t* entry = base + table + 2 * (id + 27648);
  uint32_t offset = entry[0] << 8 | entry[1];
  return offset == 0xFFFF ? 0 : table + offset;
}

// A hash (FNV-1a) of the career variable, 0 without one.
uint64_t CareerHash(const uint8_t* base) {
  uint32_t career = VariableAddress(base, kCareer), size = VariableAddress(base, kCareerSize);
  if (!career || !size) {
    return 0;
  }
  uint32_t length = std::min<uint32_t>(LoadGuest32(base, size), 0x2000);
  uint64_t hash = 0xCBF29CE484222325;
  for (uint32_t i = 0; i < length; ++i) {
    hash = (hash ^ base[career + i]) * 0x100000001B3;
  }
  return length ? hash : 0;
}

std::filesystem::path XpPath() {
  return rex::system::kernel_state()->content_manager()->ResolveGameUserContentPath() /
         kXpFileName;
}

// Under xp_mutex.
XpFile& Xp() {
  std::error_code error;
  auto time = std::filesystem::last_write_time(XpPath(), error);
  if (!xp_loaded || (!error && time != xp_time)) {
    xp_loaded = true;
    xp_time = time;
    std::ifstream file(XpPath(), std::ios::binary);
    char magic[sizeof(kXpMagic)];
    XpFile loaded;
    if (file.read(magic, sizeof(magic)) && !std::memcmp(magic, kXpMagic, sizeof(magic)) &&
        file.read(reinterpret_cast<char*>(loaded.totals.data()), sizeof(loaded.totals)) &&
        file.read(reinterpret_cast<char*>(&loaded.counted_career), sizeof(loaded.counted_career))) {
      xp = loaded;
    }
  }
  return xp;
}

void SaveXp() {
  std::filesystem::path path = XpPath();
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(kXpMagic, sizeof(kXpMagic));
    file.write(reinterpret_cast<const char*>(xp.totals.data()), sizeof(xp.totals));
    file.write(reinterpret_cast<const char*>(&xp.counted_career), sizeof(xp.counted_career));
  }
  std::filesystem::rename(temporary, path, error);
  xp_time = std::filesystem::last_write_time(path, error);
}

}  // namespace

REX_EXTERN(__imp__sub_88597528);

namespace {

struct DecisionPoint {
  uint32_t id = 0, row = 0, rank = 0;
  // Per choice, its items' types, numbers, titles' and descriptions' text
  // IDs.
  std::array<std::array<uint32_t, 4>, 2> items[2] = {};
};

std::vector<DecisionPoint> decision_points;

int ClassRow(uint32_t id) {
  switch (id) {
    case 23: return 0;
    case 26: return 1;
    case 20: return 2;
    default: return -1;
  }
}

uint32_t CallService(PPCContext& ctx, uint8_t* base, uint32_t offset, uint32_t r3, uint32_t r4) {
  uint32_t services = LoadGuest32(base, kServices);
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  rex::runtime::ResolveIndirectFunction(LoadGuest32(base, services + offset))(ctx, base);
  return ctx.r3.u32;
}

// The decision points, read from the game's unlockables once there are some.
const std::vector<DecisionPoint>& DecisionPoints(PPCContext& ctx, uint8_t* base) {
  if (!decision_points.empty() || !LoadGuest32(base, kServices)) {
    return decision_points;
  }
  uint32_t handle = LoadGuest32(base, kUnlockables);
  int32_t count = int32_t(CallService(ctx, base, kServiceCount, handle, 0));
  for (int32_t i = 0; i < count; ++i) {
    uint32_t record = CallService(ctx, base, kServiceGet, handle, uint32_t(i));
    int row = record ? ClassRow(LoadGuest32(base, record + 16)) : -1;
    if (row < 0 || !base[record + 8]) {
      continue;
    }
    DecisionPoint point;
    point.id = LoadGuest32(base, record);
    point.row = uint32_t(row);
    point.rank = LoadGuest32(base, record + 12);
    for (uint32_t item = 0; item < 4; ++item) {
      for (uint32_t field = 0; field < 4; ++field) {
        point.items[item / 2][item % 2][field] = LoadGuest32(base, record + 24 + 20 * item + 4 * field);
      }
    }
    decision_points.push_back(point);
  }
  std::sort(decision_points.begin(), decision_points.end(),
            [](const DecisionPoint& a, const DecisionPoint& b) { return a.rank < b.rank; });
  REXKRNL_INFO("System Link progression: {} decision points", decision_points.size());
  return decision_points;
}

std::filesystem::path ProfileFile(const char* name) {
  return rex::system::kernel_state()->content_manager()->ResolveGameUserContentPath() / name;
}

// The choices made: decision point ID -> 1 or 2.
std::map<uint32_t, uint32_t> Choices() {
  std::map<uint32_t, uint32_t> choices;
  std::ifstream file(ProfileFile(kChoicesFileName));
  for (uint32_t id, choice; file >> id >> choice;) {
    if (choice == 1 || choice == 2) {
      choices[id] = choice;
    }
  }
  return choices;
}

// Puts the items of the choices made into the classes' unlocked items, and
// the first decision point reached without a choice into the decision file
// (or removes it).
void ApplyDecisions(PPCContext& ctx, uint8_t* base, uint32_t progression) {
  const std::vector<DecisionPoint>& points = DecisionPoints(ctx, base);
  std::map<uint32_t, uint32_t> choices = Choices();
  const DecisionPoint* waiting = nullptr;
  for (const DecisionPoint& point : points) {
    if (base[progression + kClasses + point.row * kClassSize + kClassRank] < point.rank) {
      continue;
    }
    auto choice = choices.find(point.id);
    if (choice == choices.end()) {
      waiting = waiting ? waiting : &point;
      continue;
    }
    // As the game's own choice (sub_88175C18, DecisionPointChoice): the
    // items unlocked, and the class's last decision point rank (+112).
    for (const auto& item : point.items[choice->second - 1]) {
      if (!item[0]) {
        continue;
      }
      ctx.r3.u64 = progression + kInventories + point.row * kInventorySize;
      ctx.r4.u64 = item[0];
      ctx.r5.u64 = item[1];
      __imp__sub_88597528(ctx, base);
    }
    uint8_t& last = base[progression + kClasses + point.row * kClassSize + kClassDecision];
    if (last < point.rank) {
      last = uint8_t(point.rank);
      base[progression + kClasses + point.row * kClassSize + kClassChanged] = 1;
    }
  }
  std::string decision;
  if (waiting) {
    std::ostringstream line;
    line << waiting->id << '\t' << waiting->row << '\t' << waiting->rank;
    for (const auto& choice : waiting->items) {
      line << '\t';
      bool first = true;
      for (const auto& item : choice) {
        if (item[0] && item[2]) {
          line << (first ? "" : ",") << item[2];
          first = false;
        }
      }
    }
    for (const auto& choice : waiting->items) {
      line << '\t' << (choice[0][0] ? choice[0][3] : choice[1][3]);
    }
    for (const auto& choice : waiting->items) {
      const auto& item = choice[0][0] ? choice[0] : choice[1];
      line << '\t' << item[0] << ':' << item[1];
    }
    decision = line.str() + "\n";
  }
  std::filesystem::path path = ProfileFile(kDecisionFileName);
  std::ifstream current(path);
  std::string old((std::istreambuf_iterator<char>(current)), std::istreambuf_iterator<char>());
  current.close();
  std::error_code error;
  if (decision.empty()) {
    std::filesystem::remove(path, error);
  } else if (decision != old) {
    std::ofstream(path, std::ios::trunc) << decision;
    REXKRNL_INFO("System Link progression: decision point {} waits for a choice", waiting->id);
  }
}

struct Attachments {
  std::array<uint32_t, 3> spent{};
  // (class, weapon number) -> its attachments bought
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> bought;
};

Attachments attachments;
std::filesystem::file_time_type attachments_time{};
bool attachments_loaded = false;

// Reads the file again when something else (the player account) replaced it.
Attachments& LoadAttachments() {
  std::error_code error;
  auto time = std::filesystem::last_write_time(ProfileFile(kAttachmentsFileName), error);
  if (!attachments_loaded || (!error && time != attachments_time)) {
    attachments_loaded = true;
    attachments_time = time;
    attachments = {};
    std::ifstream file(ProfileFile(kAttachmentsFileName));
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
  }
  return attachments;
}

void SaveAttachments() {
  std::filesystem::path path = ProfileFile(kAttachmentsFileName);
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
  attachments_time = std::filesystem::last_write_time(path, error);
}

// Puts the attachments bought back into the classes' weapons, and gives
// them the credits they have left.
void ApplyAttachments(uint8_t* base, uint32_t progression) {
  Attachments& model = LoadAttachments();
  for (uint32_t row = 0; row < 3; ++row) {
    uint32_t inventory = progression + kInventories + row * kInventorySize;
    uint32_t count = LoadGuest32(base, inventory + kInventoryWeaponCount);
    uint32_t weapons = LoadGuest32(base, inventory + kInventoryWeapons);
    for (uint32_t i = 0; weapons && i < count && i < 256; ++i) {
      uint32_t weapon = weapons + i * kWeaponSize;
      auto kept = model.bought.find({row, uint32_t(base[weapon + kWeaponNumber] << 8 |
                                                   base[weapon + kWeaponNumber + 1])});
      if (kept != model.bought.end()) {
        uint64_t bought = uint64_t(LoadGuest32(base, weapon)) << 32 | LoadGuest32(base, weapon + 4);
        bought |= kept->second;
        StoreGuest32(base, weapon, uint32_t(bought >> 32));
        StoreGuest32(base, weapon + 4, uint32_t(bought));
      }
    }
    uint32_t class_row = progression + kClasses + row * kClassSize;
    uint32_t rank = base[class_row + kClassRank];
    uint16_t left = uint16_t(rank > model.spent[row] ? rank - model.spent[row] : 0);
    base[class_row + kClassCredits] = uint8_t(left >> 8);
    base[class_row + kClassCredits + 1] = uint8_t(left);
    base[class_row + kClassCreditsEarned] = uint8_t(rank >> 8);
    base[class_row + kClassCreditsEarned + 1] = uint8_t(rank);
  }
}

// Notices attachments bought (a weapon's attachment, still locked for the
// others) and keeps them, with the credits spent. A weapon's attachments when
// first seen are its own.
void TrackAttachments(uint8_t* base, uint32_t progression) {
  Attachments& model = LoadAttachments();
  bool changed = false;
  for (uint32_t row = 0; row < 3; ++row) {
    uint32_t inventory = progression + kInventories + row * kInventorySize;
    uint64_t all_locked = uint64_t(LoadGuest32(base, inventory + kInventoryLocked)) << 32 |
                          LoadGuest32(base, inventory + kInventoryLocked + 4);
    uint32_t count = LoadGuest32(base, inventory + kInventoryWeaponCount);
    uint32_t weapons = LoadGuest32(base, inventory + kInventoryWeapons);
    for (uint32_t i = 0; weapons && i < count && i < 256; ++i) {
      uint32_t weapon = weapons + i * kWeaponSize;
      uint32_t number = base[weapon + kWeaponNumber] << 8 | base[weapon + kWeaponNumber + 1];
      uint64_t bought = uint64_t(LoadGuest32(base, weapon)) << 32 | LoadGuest32(base, weapon + 4);
      // (first seen: the weapon's own attachments, not bought)
      auto [kept, added] = model.bought.try_emplace({row, number}, bought);
      changed |= added;
      uint64_t now_bought = bought & ~kept->second & all_locked;
      if (now_bought) {
        model.spent[row] += uint32_t(__builtin_popcountll(now_bought));
        kept->second |= now_bought;
        changed = true;
        REXKRNL_INFO("System Link progression: class {} bought attachments {:X} for weapon {}",
                     row, now_bought, number);
      }
    }
  }
  if (changed) {
    SaveAttachments();
  }
}

// The rank of a class with that XP, before the game caps it.
uint32_t RankForXp(const uint8_t* base, uint32_t class_xp) {
  uint32_t settings = LoadGuest32(base, kSettingsPointer);
  if (!settings) {
    return 0;
  }
  uint32_t count = base[settings + kRankCount];
  uint32_t table = LoadGuest32(base, settings + kRankXpPointer);
  uint32_t rank = 0;
  while (table && rank + 1 < count && LoadGuest32(base, table + 4 * (rank + 1)) <= class_xp) {
    ++rank;
  }
  return rank;
}

}  // namespace

REX_EXTERN(__imp__sub_881AA208);
REX_EXTERN(__imp__sub_881719E0);
REX_EXTERN(__imp__sub_8818C5C0);

// Whether the player has a Uplay Passport (with the "UplayPassport" debug
// option, or from the game executable), and whether the progression may go
// past the level cap without one: the Uplay servers are gone, so always.
REX_HOOK_RAW(sub_881ABB68) {
  ctx.r3.u64 = 1;
}

REX_HOOK_RAW(sub_88174718) {
  ctx.r3.u64 = 1;
}

// Whether the session ranks players (levels, XP, locked items): only online
// sessions do, so the gunsmith and loadouts let anything be picked offline.
REX_HOOK_RAW(sub_881AA208) {
  __imp__sub_881AA208(ctx, base);
  if (REXCVAR_GET(system_link_progression)) {
    ctx.r3.u64 = 1;
  }
}

REX_EXTERN(__imp__sub_881729E8);
REX_EXTERN(sub_881719E0);

// Refreshes the player's progression (r3) from the servers when logged in to
// them: resets it, marks a refresh pending (+0x10BC8) and queues the request,
// whose answer clears the mark. In System Link the player only seems logged in
// (for the progression) and no answer ever comes: the characters menu then
// waits forever ("Refreshing saved customization data"). So no refresh, as
// when not logged in; the classes keep their totals. What they unlock is
// worked out again, with the decision points chosen since.
REX_HOOK_RAW(sub_881729E8) {
  static const bool system_link = rex::cvar::Query<bool>("system_link");
  if (system_link && REXCVAR_GET(system_link_progression)) {
    sub_881719E0(ctx, base);
    return;
  }
  __imp__sub_881729E8(ctx, base);
}

// Works out what the player's progression (r3) unlocks from the ranks of its
// classes, after the game resets or loads it. First, if the classes hold a
// match result not added up yet, the totals take it in; then each
// class gets its total XP and its rank, capped as the game does; after the
// game's own unlocks, the items of the decision points chosen.
REX_HOOK_RAW(sub_881719E0) {
  if (REXCVAR_GET(system_link_progression)) {
    uint32_t progression = ctx.r3.u32;
    std::lock_guard<std::mutex> lock(xp_mutex);
    XpFile& file = Xp();
    uint64_t career = CareerHash(base);
    bool loaded_result = false;
    for (uint32_t i = 0; i < kClassCount; ++i) {
      loaded_result |= LoadGuest32(base, progression + kClasses + i * kClassSize + kClassXp) != 0;
    }
    if (career && career != file.counted_career && loaded_result) {
      for (uint32_t i = 0; i < kClassCount; ++i) {
        // The game adds the XP of a match to what the class had then: its
        // total, as restored below, or nothing if the game had reset it.
        uint32_t loaded = LoadGuest32(base, progression + kClasses + i * kClassSize + kClassXp);
        uint32_t total = loaded >= file.totals[i] ? loaded : file.totals[i] + loaded;
        if (total != file.totals[i]) {
          REXKRNL_INFO("System Link progression: class {} earned {} XP", i,
                       total - file.totals[i]);
        }
        file.totals[i] = total;
      }
      file.counted_career = career;
      SaveXp();
    }
    for (uint32_t i = 0; i < kClassCount; ++i) {
      uint32_t row = progression + kClasses + i * kClassSize;
      StoreGuest32(base, row + kClassXp, file.totals[i]);
      // The game's highest rank for one (with the DLC owned; 5, its lowest,
      // without a Uplay Passport): sub_8818C5C0.
      uint32_t rank = RankForXp(base, file.totals[i]);
      ctx.r3.u64 = rank;
      __imp__sub_8818C5C0(ctx, base);
      base[row + kClassRank] = uint8_t(std::min(rank, ctx.r3.u32));
    }
    ctx.r3.u64 = progression;
    __imp__sub_881719E0(ctx, base);
    ApplyDecisions(ctx, base, progression);
    ApplyAttachments(base, progression);
    return;
  }
  __imp__sub_881719E0(ctx, base);
}

REX_EXTERN(__imp__sub_88174E30);

// Works out again what a class (r4) of the progression (r3) unlocks with its
// rank, from nothing: the decision points' items go back in after it.
REX_HOOK_RAW(sub_88174E30) {
  uint32_t progression = ctx.r3.u32;
  __imp__sub_88174E30(ctx, base);
  if (REXCVAR_GET(system_link_progression)) {
    PPCRegister r3 = ctx.r3;
    ApplyDecisions(ctx, base, progression);
    ApplyAttachments(base, progression);
    ctx.r3 = r3;
  }
}

REX_EXTERN(__imp__sub_884EEA08);

// Runs a command of the menus' Flash screens. A decision point chosen on the
// port's screen since the last one goes into the progression (the game's
// own, *kProgression) before it, so the menus show its items at once; the
// attachments it bought are kept after it.
REX_HOOK_RAW(sub_884EEA08) {
  static std::filesystem::file_time_type applied{};
  static const bool system_link = rex::cvar::Query<bool>("system_link");
  std::error_code error;
  auto time = std::filesystem::last_write_time(ProfileFile(kChoicesFileName), error);
  uint32_t progression = LoadGuest32(base, kProgression);
  const bool enabled = system_link && REXCVAR_GET(system_link_progression) && progression;
  if (enabled && !error && time != applied) {
    applied = time;
    PPCContext saved = ctx;
    ApplyDecisions(ctx, base, progression);
    ApplyAttachments(base, progression);
    ctx = saved;
  }
  __imp__sub_884EEA08(ctx, base);
  // (the gunsmith buys attachments on its commands)
  if (enabled) {
    TrackAttachments(base, progression);
  }
}

REX_EXTERN(__imp__sub_8815B1C0);
REX_EXTERN(__imp__sub_8815C0E8);

// Starts the Ghost Feed (r3) when the player is logged in to Ubisoft's
// servers, which ask for its news. With news of our own (news_url, in
// src/news_hooks.cpp), it starts anyway.
REX_HOOK_RAW(sub_8815B1C0) {
  uint32_t feed = ctx.r3.u32;
  __imp__sub_8815B1C0(ctx, base);
  static const bool own_news = !rex::cvar::Query<std::string>("news_url").empty();
  // Flags: +3272 bit 32 (the feed is off), +3273 bit 4 (started).
  if (own_news && !(base[feed + 3272] & 32) && !(base[feed + 3273] & 4)) {
    ctx.r3.u64 = feed;
    __imp__sub_8815C0E8(ctx, base);
  }
}
