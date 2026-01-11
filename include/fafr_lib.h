#pragma once

#include <cstdint>
#include <string>

static constexpr uint32_t FAFR_MAGIC = 0x52464146; // "FAFR" little-endian
static constexpr uint32_t FAFR_VERSION = 1;

#pragma pack(push, 1)
struct FafrHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t dtype;       // 1 = complex float32
  uint32_t frame_size;
  uint32_t hop_size;
  uint64_t total_samples_per_channel;
  uint64_t total_frames;
};
#pragma pack(pop)

struct FafrEncodeOptions {
  uint32_t frame_size = 4096;
  uint32_t hop_size = 1024;
  float fade_ms = 20.0f;
};

void fafr_encode_wav_to_fafr(const std::string& in_path,
                             const std::string& out_path,
                             const FafrEncodeOptions& opt);

void fafr_decode_fafr_to_wav(const std::string& in_path,
                             const std::string& out_path);
