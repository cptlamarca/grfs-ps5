// The Ghost Feed's news, from a server of our own: the game asked Ubisoft's
// (Quazal "Overlord") servers for them, which are gone. With news_url set, a
// request for news downloads that address over HTTP and hands its messages to
// the game as the servers' answer (tools/news_server.py serves and edits them).
//
// The file has one message per line, its fields separated by tabs:
//   YYYY-MM-DD HH:MM <tab> category <tab> author <tab> title <tab> text
// with "\n", "\t" and "\\" escaped. The category (SP, MP, CIS, WEB, MOB, XBLA
// or nothing) picks the feed's tab.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_STRING(news_url, "", "Online",
                      "Address (http://host:port/path) of the Ghost Feed's news, served by "
                      "tools/news_server.py; empty keeps the game's own (unreachable) servers");

namespace {

struct News {
  int year = 2012, month = 1, day = 1, hour = 0, minute = 0;
  std::string category, author, title, text;
};

// How long downloaded news are shown before a request downloads them again.
constexpr auto kNewsRefresh = std::chrono::seconds(60);

std::mutex news_mutex;
bool news_fetching = false;
bool news_fetched = false, news_ok = false;
std::chrono::steady_clock::time_point news_time;
std::vector<News> news;
// Guest memory of the strings last handed to the game, and of a status word.
std::vector<uint32_t> news_strings;
uint32_t news_status = 0;

std::string Unescape(const std::string& field) {
  std::string out;
  for (size_t i = 0; i < field.size(); ++i) {
    if (field[i] == '\\' && i + 1 < field.size()) {
      char c = field[++i];
      out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
    } else {
      out += field[i];
    }
  }
  return out;
}

std::vector<News> ParseNews(const std::string& body) {
  std::vector<News> items;
  std::istringstream lines(body);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1) {
      fields.push_back(line.substr(start, tab - start));
    }
    fields.push_back(line.substr(start));
    if (fields.size() < 5) {
      continue;
    }
    News item;
    std::sscanf(fields[0].c_str(), "%d-%d-%d %d:%d", &item.year, &item.month, &item.day,
                &item.hour, &item.minute);
    item.category = Unescape(fields[1]);
    item.author = Unescape(fields[2]);
    item.title = Unescape(fields[3]);
    item.text = Unescape(fields[4]);
    items.push_back(std::move(item));
  }
  return items;
}

// A plain HTTP/1.0 GET; false if the server can't be reached or doesn't
// answer 200.
bool HttpGet(const std::string& url, std::string& body) {
  std::string rest = url;
  if (rest.rfind("http://", 0) == 0) {
    rest = rest.substr(7);
  }
  size_t slash = rest.find('/');
  std::string host_port = rest.substr(0, slash);
  std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
  std::string host = host_port, port = "80";
  if (size_t colon = host_port.rfind(':'); colon != std::string::npos) {
    host = host_port.substr(0, colon);
    port = host_port.substr(colon + 1);
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(uint16_t(std::stoi(port)));
  if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found) {
      return false;
    }
    address.sin_addr = reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr;
    freeaddrinfo(found);
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  timeval timeout{10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  std::string response;
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
    std::string request =
        "GET " + path + " HTTP/1.0\r\nHost: " + host_port + "\r\nConnection: close\r\n\r\n";
    if (send(fd, request.data(), request.size(), 0) == ssize_t(request.size())) {
      char buffer[4096];
      for (ssize_t n; (n = recv(fd, buffer, sizeof(buffer), 0)) > 0;) {
        response.append(buffer, size_t(n));
      }
    }
  }
  close(fd);

  size_t header_end = response.find("\r\n\r\n");
  if (header_end == std::string::npos || response.find(" 200 ") > response.find("\r\n")) {
    return false;
  }
  body = response.substr(header_end + 4);
  return true;
}

// Under news_mutex.
void StartFetch() {
  if (news_fetching) {
    return;
  }
  news_fetching = true;
  std::thread([url = std::string(REXCVAR_GET(news_url))] {
    std::string body;
    bool ok = HttpGet(url, body);
    std::vector<News> items = ok ? ParseNews(body) : std::vector<News>();
    std::lock_guard<std::mutex> lock(news_mutex);
    if (ok) {
      news = std::move(items);
      REXKRNL_INFO("News: {} messages from {}", news.size(), url);
    } else {
      REXKRNL_WARN("News: can't download {}", url);
    }
    news_ok = ok;
    news_fetched = true;
    news_fetching = false;
    news_time = std::chrono::steady_clock::now();
  }).detach();
}

uint32_t GuestString(uint8_t* base, const std::string& text) {
  auto* memory = rex::system::kernel_state()->memory();
  uint32_t address = memory->SystemHeapAlloc(uint32_t(text.size() + 1));
  if (address) {
    std::memcpy(base + address, text.c_str(), text.size() + 1);
    news_strings.push_back(address);
  }
  return address;
}

void StoreGuest32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

}  // namespace

REX_EXTERN(__imp__sub_8243CD70);
REX_EXTERN(__imp__sub_8243C600);
REX_EXTERN(__imp__sub_8243C6E0);

// Asks the servers for news (r3: the Overlord client); the answer comes back
// as an event, from sub_8243C600 with the request's status. Until the news are
// downloaded, the request fails, which the Ghost Feed retries.
REX_HOOK_RAW(sub_8243CD70) {
  if (std::string(REXCVAR_GET(news_url)).empty()) {
    __imp__sub_8243CD70(ctx, base);
    return;
  }
  uint32_t overlord = ctx.r3.u32;
  bool ok;
  {
    std::lock_guard<std::mutex> lock(news_mutex);
    if (!news_fetched || std::chrono::steady_clock::now() - news_time > kNewsRefresh) {
      StartFetch();
    }
    if (!news_fetched) {
      ctx.r3.u64 = 0;
      return;
    }
    ok = news_ok;
    if (!news_status) {
      news_status = rex::system::kernel_state()->memory()->SystemHeapAlloc(4);
    }
    StoreGuest32(base, news_status, ok ? 0 : 1);
  }
  ctx.r3.u64 = overlord;
  ctx.r4.u64 = news_status;
  __imp__sub_8243C600(ctx, base);
  ctx.r3.u64 = 1;
}

// Copies the news (r3: the Overlord client) to r4, up to *r5 of 52 bytes:
// the author, the title (after its category in brackets) and the text, as
// strings, the author's player id (0: none), and the time as struct tm's
// first six fields. *r5 gets the number of news copied.
REX_HOOK_RAW(sub_8243C6E0) {
  if (std::string(REXCVAR_GET(news_url)).empty()) {
    __imp__sub_8243C6E0(ctx, base);
    return;
  }
  uint32_t out = ctx.r4.u32, count_address = ctx.r5.u32;
  if (!count_address) {
    ctx.r3.u64 = 0;
    return;
  }
  std::lock_guard<std::mutex> lock(news_mutex);
  auto* memory = rex::system::kernel_state()->memory();
  for (uint32_t address : news_strings) {
    memory->SystemHeapFree(address);
  }
  news_strings.clear();
  uint32_t count = std::min<uint32_t>(LoadGuest32(base, count_address), uint32_t(news.size()));
  for (uint32_t i = 0; out && i < count; ++i) {
    const News& item = news[i];
    uint32_t entry = out + 52 * i;
    std::memset(base + entry, 0, 52);
    // The feed shows the title of the selected news only: the text goes there
    // too, after it.
    std::string title = item.title.empty() || item.text.empty() ? item.title + item.text
                                                                 : item.title + "\n" + item.text;
    if (!item.category.empty()) {
      title = "[" + item.category + "]" + title;
    }
    StoreGuest32(base, entry + 0, GuestString(base, item.author));
    StoreGuest32(base, entry + 4, GuestString(base, title));
    StoreGuest32(base, entry + 8, GuestString(base, item.text));
    const int time[6] = {0, item.minute, item.hour, item.day, item.month - 1, item.year - 1900};
    for (int j = 0; j < 6; ++j) {
      StoreGuest32(base, entry + 16 + 4 * j, uint32_t(time[j]));
    }
  }
  StoreGuest32(base, count_address, out ? count : uint32_t(news.size()));
  ctx.r3.u64 = 1;
}
