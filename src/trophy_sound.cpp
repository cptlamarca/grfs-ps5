// The trophy sound, through a sceAudioOut port of its own next to the game's.

#include "trophy_sound.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, audio_mute);

#if defined(__SCE__)
extern "C" {
int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                        uint32_t param);
int32_t sceAudioOutOutput(int32_t handle, const void* ptr);
int32_t sceAudioOutClose(int32_t handle);
}
#endif

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kGrain = 256;

// Interleaved stereo samples at 48 kHz.
using Stereo = std::vector<float>;

uint32_t ReadLE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t ReadLE16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

bool LoadWav(const std::filesystem::path& path, Stereo& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) || std::memcmp(data.data() + 8, "WAVE", 4)) {
    return false;
  }
  uint16_t channels = 0, bits = 0, format = 0;
  uint32_t rate = 0;
  const uint8_t* samples = nullptr;
  size_t samples_size = 0;
  for (size_t at = 12; at + 8 <= data.size();) {
    uint32_t size = ReadLE32(data.data() + at + 4);
    const uint8_t* chunk = data.data() + at + 8;
    if (at + 8 + size > data.size()) {
      size = uint32_t(data.size() - at - 8);
    }
    if (!std::memcmp(data.data() + at, "fmt ", 4) && size >= 16) {
      format = ReadLE16(chunk);
      channels = ReadLE16(chunk + 2);
      rate = ReadLE32(chunk + 4);
      bits = ReadLE16(chunk + 14);
    } else if (!std::memcmp(data.data() + at, "data", 4)) {
      samples = chunk;
      samples_size = size;
    }
    at += 8 + size + (size & 1);
  }
  if (format != 1 || bits != 16 || !rate || (channels != 1 && channels != 2) || !samples) {
    REXLOG_WARN("{}: only 16-bit PCM WAV files are played", path.string());
    return false;
  }
  size_t frames = samples_size / (2 * channels);
  auto sample = [&](size_t frame, int channel) {
    return int16_t(ReadLE16(samples + (frame * channels + std::min(channel, channels - 1)) * 2)) /
           32768.0f;
  };
  // Linear resampling to 48 kHz.
  size_t out_frames = size_t(double(frames) * kRate / rate);
  out.resize(out_frames * 2);
  for (size_t i = 0; i < out_frames; ++i) {
    double position = double(i) * rate / kRate;
    size_t frame = std::min(size_t(position), frames - 1);
    size_t next = std::min(frame + 1, frames - 1);
    float t = float(position - double(frame));
    for (int c = 0; c < 2; ++c) {
      out[i * 2 + c] = sample(frame, c) * (1.0f - t) + sample(next, c) * t;
    }
  }
  return true;
}

// Two bell-like notes going up.
Stereo Chime() {
  struct Note {
    float start, frequency;
  };
  constexpr Note kNotes[] = {{0.0f, 1046.5f}, {0.11f, 1568.0f}};
  constexpr float kLength = 1.3f;
  Stereo out(size_t(kLength * kRate) * 2);
  for (size_t i = 0; i < out.size() / 2; ++i) {
    float time = float(i) / kRate;
    float value = 0.0f;
    for (const Note& note : kNotes) {
      float t = time - note.start;
      if (t < 0.0f) {
        continue;
      }
      float envelope = std::min(t / 0.004f, 1.0f) * std::exp(-t / 0.32f);
      float phase = 2.0f * float(M_PI) * note.frequency * t;
      value += envelope * (std::sin(phase) + 0.35f * std::sin(2.0f * phase) * std::exp(-t / 0.12f) +
                           0.12f * std::sin(3.01f * phase) * std::exp(-t / 0.06f));
    }
    out[i * 2] = out[i * 2 + 1] = value * 0.22f;
  }
  return out;
}

void Play(Stereo sound) {
#if defined(__SCE__)
  constexpr int32_t kSystemUser = 0xFF;
  constexpr uint32_t kFormatS16Stereo = 1;
  // A voice port, beside the game's main one (the main port as a fallback).
  int32_t port = sceAudioOutOpen(kSystemUser, 2, 0, kGrain, kRate, kFormatS16Stereo);
  if (port < 0) {
    port = sceAudioOutOpen(kSystemUser, 0, 1, kGrain, kRate, kFormatS16Stereo);
  }
  if (port < 0) {
    REXLOG_WARN("Trophy sound: sceAudioOutOpen failed ({:#x})", uint32_t(port));
    return;
  }
  int16_t pcm[kGrain * 2];
  for (size_t at = 0; at < sound.size(); at += kGrain * 2) {
    for (size_t i = 0; i < kGrain * 2; ++i) {
      float s = at + i < sound.size() ? sound[at + i] : 0.0f;
      pcm[i] = int16_t(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
    }
    sceAudioOutOutput(port, pcm);
  }
  sceAudioOutOutput(port, nullptr);  // (waits for the last buffer)
  sceAudioOutClose(port);
#else
  (void)sound;
#endif
}

}  // namespace

void PlayTrophySound(const std::filesystem::path& game_data_root) {
  if (REXCVAR_GET(audio_mute)) {
    return;
  }
  std::filesystem::path path = game_data_root / "metadata" / "trophy.wav";
  std::thread([path] {
    Stereo sound;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !LoadWav(path, sound)) {
      sound = Chime();
    }
    Play(std::move(sound));
  }).detach();
}
