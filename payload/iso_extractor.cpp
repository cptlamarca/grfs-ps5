// The ISO extractor payload: the app (src/iso_screen.cpp) sends it to the
// payload loader on port 9021 (etaHEN, elfldr, ...), where, as a process of
// its own, it writes at the drive's full speed (the PS5 slows an app's
// writes to a few MB a second after its first GBs).
//
// /data/grfs-iso/request.txt: the request's ID, the disc image, the folder
// to extract it into and the root file to write last, a line each.
// /data/grfs-iso/status.txt, rewritten as it goes: "running|done ID DONE
// TOTAL" and the file being written, or "error ID" and the message.
// /data/grfs-iso/cancel stops it.

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "../src/disc_image.h"

namespace {

constexpr char kFolder[] = "/data/grfs-iso";

std::string Path(const char* name) { return std::string(kFolder) + "/" + name; }

void WriteStatus(const std::string& text) {
  const std::string temporary = Path("status.tmp");
  {
    std::ofstream out(temporary, std::ios::trunc);
    out << text;
  }
  std::rename(temporary.c_str(), Path("status.txt").c_str());
}

void Log(const std::string& text) {
  std::ofstream(Path("payload.log"), std::ios::app) << text << "\n";
}

}  // namespace

int main() {
  mkdir(kFolder, 0777);
  std::string id, image, folder, last;
  {
    std::ifstream in(Path("request.txt"));
    std::getline(in, id);
    std::getline(in, image);
    std::getline(in, folder);
    std::getline(in, last);
  }
  if (id.empty() || image.empty() || folder.empty()) {
    Log("no request");
    return 1;
  }
  Log("request " + id + ": " + image + " -> " + folder);
  unlink(Path("cancel").c_str());

  std::vector<DiscFile> files;
  std::string error;
  if (!ListDiscImage(image, files, error)) {
    WriteStatus("error " + id + "\n" + error + "\n");
    Log("error: " + error);
    return 1;
  }
  uint64_t total = 0;
  for (const DiscFile& file : files) {
    total += file.directory ? 0 : file.size;
  }

  ExtractProgress progress;
  std::mutex mutex;
  std::string current;
  std::atomic<bool> finished{false};
  // The status twice a second, and the cancel file's check.
  std::thread reporter([&] {
    while (!finished) {
      if (access(Path("cancel").c_str(), F_OK) == 0) {
        progress.cancel = true;
      }
      std::string file;
      {
        std::lock_guard<std::mutex> lock(mutex);
        file = current;
      }
      WriteStatus("running " + id + " " + std::to_string(progress.done.load()) + " " +
                  std::to_string(total) + "\n" + file + "\n");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  });
  const auto start = std::chrono::steady_clock::now();
  const bool ok = ExtractDiscImage(
      image, files, folder, last, progress,
      [&](const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex);
        current = path;
      },
      error);
  finished = true;
  reporter.join();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (ok) {
    WriteStatus("done " + id + " " + std::to_string(progress.done.load()) + " " +
                std::to_string(total) + "\n\n");
    char line[96];
    std::snprintf(line, sizeof(line), "done: %.2f GB in %.0f s (%.1f MB/s)",
                  progress.done / 1e9, seconds, progress.done / 1e6 / std::max(seconds, 0.001));
    Log(line);
  } else {
    WriteStatus("error " + id + "\n" + error + "\n");
    Log("error: " + error);
  }
  unlink(Path("cancel").c_str());
  return ok ? 0 : 1;
}
