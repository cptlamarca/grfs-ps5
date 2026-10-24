// See shader_seed.h. The storage files (<title>.xsh and <title>.<path>.vk.xpso)
// are a header, then records the runtime checks by their XXH3 hash:
// - .xsh (8-byte header): u64 hash of the ucode, u32 type and dword count
//   (the low 31 bits), the ucode;
// - .xpso (12-byte header): u64 hash of the description, the description,
//   all of one size.

#include "shader_seed.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/hash.h>
#include <rex/logging.h>

namespace {

struct Record {
  uint64_t hash;
  size_t offset, size;
};

std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream data;
  data << file.rdbuf();
  return data.str();
}

uint64_t U64(const std::string& data, size_t offset) {
  uint64_t value;
  std::memcpy(&value, data.data() + offset, sizeof(value));
  return value;
}

// The valid records, up to the first damaged one (where the runtime stops).
std::vector<Record> ShaderRecords(const std::string& data) {
  std::vector<Record> records;
  for (size_t at = 8; at + 12 <= data.size();) {
    uint32_t packed;
    std::memcpy(&packed, data.data() + at + 8, sizeof(packed));
    size_t size = 12 + size_t(packed & 0x7FFFFFFF) * 4;
    if (at + size > data.size() ||
        XXH3_64bits(data.data() + at + 12, size - 12) != U64(data, at)) {
      break;
    }
    records.push_back({U64(data, at), at, size});
    at += size;
  }
  return records;
}

std::vector<Record> PipelineRecords(const std::string& data, size_t size) {
  std::vector<Record> records;
  for (size_t at = 12; size && at + size <= data.size(); at += size) {
    if (XXH3_64bits(data.data() + at + 8, size - 8) != U64(data, at)) {
      break;
    }
    records.push_back({U64(data, at), at, size});
  }
  return records;
}

// A pipeline record's size: the one whose first record's hash checks out.
size_t PipelineRecordSize(const std::string& data) {
  for (size_t size = 16; 12 + size <= data.size(); ++size) {
    if ((data.size() - 12) % size == 0 &&
        XXH3_64bits(data.data() + 12 + 8, size - 8) == U64(data, 12)) {
      return size;
    }
  }
  return 0;
}

void Merge(const std::filesystem::path& seed_path, const std::filesystem::path& path) {
  const bool shaders = seed_path.extension() == ".xsh";
  const size_t header = shaders ? 8 : 12;
  const std::string seed = Read(seed_path);
  if (seed.size() <= header) {
    return;
  }
  std::string own = Read(path);
  // Another format version (or none yet): the runtime would drop it anyway.
  if (own.size() < header || own.compare(0, header, seed, 0, header) != 0) {
    own = seed.substr(0, header);
  }
  const size_t size = shaders ? 0 : PipelineRecordSize(seed);
  std::vector<Record> own_records = shaders ? ShaderRecords(own) : PipelineRecords(own, size);
  std::vector<Record> seed_records = shaders ? ShaderRecords(seed) : PipelineRecords(seed, size);
  std::unordered_set<uint64_t> known;
  std::string merged = own.substr(0, header);
  for (const Record& record : own_records) {
    known.insert(record.hash);
    merged.append(own, record.offset, record.size);
  }
  size_t added = 0;
  for (const Record& record : seed_records) {
    if (known.insert(record.hash).second) {
      merged.append(seed, record.offset, record.size);
      ++added;
    }
  }
  if (merged == own) {
    return;
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(merged.data(), std::streamsize(merged.size()));
  }
  std::filesystem::rename(temporary, path, error);
  REXLOG_INFO("Shaders: {} - {} added to {}{}", seed_path.filename().string(), added,
              own_records.size(), error ? " - failed: " + error.message() : "");
}

}  // namespace

void MergeShaderSeed(const std::filesystem::path& seed_folder,
                     const std::filesystem::path& cache_root) {
  std::error_code error;
  const std::filesystem::path storage = cache_root / "shaders" / "shareable";
  for (const auto& entry : std::filesystem::directory_iterator(seed_folder, error)) {
    const std::filesystem::path path = entry.path();
    if (path.extension() == ".xsh" || path.extension() == ".xpso") {
      Merge(path, storage / path.filename());
    }
  }
}
