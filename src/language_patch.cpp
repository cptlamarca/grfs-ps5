// See language_patch.h. An Oasis_*.inf has four header lines (the version and
// date, NbElementsPerType, BytesCountPerType and one more), then "ID:type:mask:text" lines:
// BytesCountPerType holds, per type, the UTF-8 sizes + 1 of the texts of mask
// 0, which the game checks.

#include "language_patch.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>

namespace {

// Longest first; the Xbox and Microsoft names become PlayStation and Sony ones.
const std::pair<const char*, const char*> kRebrand[] = {
    {"Xbox LIVE Marketplace", "PlayStation Store"},
    {"Xbox LIVE Gold", "PlayStation Plus"},
    {"Xbox LIVE Silver", "PlayStation Network"},
    {"Xbox LIVE", "PlayStation Network"},
    {"XBOX LIVE", "PLAYSTATION NETWORK"},
    {"Xbox 360", "PlayStation 5"},
    {"XBOX 360", "PLAYSTATION 5"},
    {"XBOX", "PLAYSTATION"},
    {"Xbox", "PlayStation"},
    {"MICROSOFT", "SONY"},
    {"Microsoft", "Sony"},
};

// The port's texts, by string ID: Portuguese (Brazil) and English (the
// other languages).
struct Text {
  const char* id;
  const char* portuguese;
  const char* english;
};
const Text kTexts[] = {
    {"83117", "// JOGOS DISPONÍVEIS", "// AVAILABLE GAMES"},
    {"106024", "AJUDE A ME MANTER ATIVO", "HELP KEEP ME ACTIVE"},
    {"107219", "DOADORES", "DONORS"},
    {"114484", "Rede Local", "Local Network"},
    {"114671", "Ver quem apoia o projeto", "See who supports the project"},
    {"116215", "Online (VPS)", "Online (VPS)"},
    {"118632", "Veja seus troféus", "View your trophies"},
    {"124354", "Apoie a continuidade do projeto", "Support the project"},
};

std::string Patch(const std::string& original, bool portuguese) {
  std::vector<std::string> lines;
  std::istringstream stream(original);
  for (std::string line; std::getline(stream, line);) {
    lines.push_back(line);
  }
  if (lines.size() < 4) {
    return original;
  }
  std::map<std::string, std::string> texts;
  for (const Text& text : kTexts) {
    texts[text.id] = portuguese ? text.portuguese : text.english;
  }
  // (case-insensitive for "Xbox LIVE" only, as in the original tool)
  static const std::regex live("Xbox LIVE", std::regex::icase);
  std::map<int, int> counts, sizes;
  for (size_t i = 4; i < lines.size(); ++i) {
    std::string& line = lines[i];
    const bool cr = !line.empty() && line.back() == '\r';
    if (cr) {
      line.pop_back();
    }
    for (const auto& [from, to] : kRebrand) {
      if (std::string(from) == "Xbox LIVE") {
        line = std::regex_replace(line, live, to);
      } else {
        for (size_t at; (at = line.find(from)) != std::string::npos;) {
          line.replace(at, std::strlen(from), to);
        }
      }
    }
    // ID:type:mask:text
    size_t a = line.find(':'), b = a == std::string::npos ? a : line.find(':', a + 1);
    size_t c = b == std::string::npos ? b : line.find(':', b + 1);
    if (!line.empty() && line.rfind("//", 0) != 0 && c != std::string::npos) {
      const std::string id = line.substr(0, a);
      const int type = std::atoi(line.substr(a + 1, b - a - 1).c_str());
      const bool plain = line.substr(b + 1, c - b - 1) == "0";
      if (plain) {
        if (auto it = texts.find(id); it != texts.end()) {
          line = line.substr(0, c + 1) + it->second;
        }
        sizes[type] += int(line.size() - c - 1) + 1;
      }
      ++counts[type];
    }
    if (cr) {
      line += '\r';
    }
  }
  const std::string eol = !lines[1].empty() && lines[1].back() == '\r' ? "\r" : "";
  int kinds = counts.empty() ? 0 : counts.rbegin()->first + 1;
  std::string count_line = "NbElementsPerType=", size_line = "BytesCountPerType=";
  for (int k = 0; k < kinds; ++k) {
    count_line += std::to_string(counts[k]) + ":";
    size_line += std::to_string(sizes[k]) + ":";
  }
  lines[1] = count_line + eol;
  lines[2] = size_line + eol;
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    out += lines[i];
    if (i + 1 < lines.size() || (!original.empty() && original.back() == '\n')) {
      out += '\n';
    }
  }
  return out;
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

}  // namespace

void PatchLanguageFiles(const std::filesystem::path& game_data_root) {
  std::error_code error;
  const std::filesystem::path folder = game_data_root / "Language";
  for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
    const std::filesystem::path path = entry.path();
    const std::string name = path.filename().string();
    if (name.rfind("Oasis_", 0) != 0 || path.extension() != ".inf") {
      continue;
    }
    // The game's own file is kept as .orig, the patch always made from it.
    std::filesystem::path original = path;
    original += ".orig";
    if (!std::filesystem::exists(original, error)) {
      std::filesystem::copy_file(path, original, error);
      if (error) {
        REXLOG_WARN("Language: can't keep {} ({})", name, error.message());
        continue;
      }
    }
    const std::string patched = Patch(Read(original), name == "Oasis_Brazil.inf");
    if (patched == Read(path)) {
      continue;
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      file.write(patched.data(), std::streamsize(patched.size()));
    }
    std::filesystem::rename(temporary, path, error);
    REXLOG_INFO("Language: {} patched{}", name, error ? " - failed: " + error.message() : "");
  }
}
