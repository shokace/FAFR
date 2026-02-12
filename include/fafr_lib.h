#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

struct FafrDecodedAudio {
  uint32_t sample_rate = 0;
  uint16_t channels = 0;
  uint64_t total_frames = 0;
  std::vector<float> interleaved;
};

struct FafrEquationOptions {
  uint32_t max_terms = 32;
};

void fafr_encode_wav_to_fafr(const std::string& in_path,
                             const std::string& out_path,
                             const FafrEncodeOptions& opt);

void fafr_decode_fafr_to_wav(const std::string& in_path,
                             const std::string& out_path);

void fafr_export_wav_equation(const std::string& in_path,
                              const std::string& out_path,
                              const FafrEquationOptions& opt);

bool fafr_parse_header_bytes(const uint8_t* data, size_t size, FafrHeader* out_hdr);

FafrDecodedAudio fafr_decode_fafr_bytes(const uint8_t* data, size_t size);

FafrDecodedAudio fafr_decode_fafr_coeffs(const FafrHeader& hdr,
                                         const uint8_t* coeff_bytes,
                                         size_t coeff_size);
