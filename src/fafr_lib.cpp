#include "fafr_lib.h"

#include <sndfile.h>
#include <fftw3.h>

#include <cmath>
#include <memory>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <algorithm>

static std::vector<float> hann_window(uint32_t N) {
  std::vector<float> w(N);
  // Standard Hann: w[n] = 0.5 - 0.5*cos(2*pi*n/(N-1))
  for (uint32_t n = 0; n < N; n++) {
    w[n] = 0.5f - 0.5f * std::cos(float(2.0 * M_PI) * float(n) / float(N - 1));
  }
  return w;
}

static void apply_fade_in_out(std::vector<float>& interleaved, uint16_t ch, uint32_t sr, float fade_ms) {
  if (fade_ms <= 0.0f) return;

  const uint64_t total_frames = interleaved.size() / ch;
  const uint64_t fade_frames = static_cast<uint64_t>(std::llround((fade_ms / 1000.0) * sr));
  const uint64_t F = std::min<uint64_t>(fade_frames, total_frames / 2);

  // Linear fade in/out to exactly hit zero at ends.
  for (uint64_t i = 0; i < F; i++) {
    float g_in  = float(i) / float(std::max<uint64_t>(1, F - 1));
    float g_out = float(F - 1 - i) / float(std::max<uint64_t>(1, F - 1));

    // fade-in at start
    for (uint16_t c = 0; c < ch; c++) {
      interleaved[i * ch + c] *= g_in;
    }
    // fade-out at end
    uint64_t j = (total_frames - F + i);
    for (uint16_t c = 0; c < ch; c++) {
      interleaved[j * ch + c] *= g_out;
    }
  }
}

void fafr_encode_wav_to_fafr(const std::string& in_path,
                             const std::string& out_path,
                             const FafrEncodeOptions& opt) {
  SF_INFO info{};
  SNDFILE* in = sf_open(in_path.c_str(), SFM_READ, &info);
  if (!in) throw std::runtime_error(std::string("sf_open read failed: ") + sf_strerror(nullptr));
  if (info.frames <= 0) throw std::runtime_error("Input WAV has no frames.");
  if (info.channels <= 0) throw std::runtime_error("Invalid channel count.");

  const uint16_t channels = static_cast<uint16_t>(info.channels);
  const uint32_t sr = static_cast<uint32_t>(info.samplerate);
  const uint64_t total_frames = static_cast<uint64_t>(info.frames);

  std::vector<float> audio_interleaved(total_frames * channels);
  sf_count_t got = sf_readf_float(in, audio_interleaved.data(), info.frames);
  sf_close(in);
  if (got != info.frames) throw std::runtime_error("Failed to read all samples.");

  // Flatten ends to x-axis via fade
  apply_fade_in_out(audio_interleaved, channels, sr, opt.fade_ms);

  const uint32_t N = opt.frame_size;
  const uint32_t H = opt.hop_size;
  const uint32_t bins = N / 2 + 1;

  // Number of frames for coverage (include last partial, zero-pad)
  uint64_t num_frames = 0;
  if (total_frames <= 1) {
    num_frames = 1;
  } else {
    // start positions: 0, H, 2H, ... <= total_frames-1
    // We'll include any frame whose start < total_frames
    num_frames = (total_frames + H - 1) / H;
  }

  auto w = hann_window(N);

  // Prepare FFTW plans (per channel reuse)
  std::vector<double> timebuf(N);
  std::unique_ptr<fftw_complex, decltype(&fftw_free)> freqbuf(
      fftw_alloc_complex(bins), &fftw_free);
  if (!freqbuf) throw std::runtime_error("fftw_alloc_complex failed.");
  fftw_plan plan = fftw_plan_dft_r2c_1d(
      (int)N, timebuf.data(), freqbuf.get(), FFTW_ESTIMATE);

  // Open output
  std::ofstream out(out_path, std::ios::binary);
  if (!out) throw std::runtime_error("Failed to open output .fafr for writing.");

  FafrHeader hdr{};
  hdr.magic = FAFR_MAGIC;
  hdr.version = FAFR_VERSION;
  hdr.sample_rate = sr;
  hdr.channels = channels;
  hdr.dtype = 1; // complex float32
  hdr.frame_size = N;
  hdr.hop_size = H;
  hdr.total_samples_per_channel = total_frames;
  hdr.total_frames = num_frames;

  out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

  // For each frame and channel: store complex float32 bins
  // Layout: frame0 ch0 bins..., ch1 bins..., frame1 ...
  for (uint64_t f = 0; f < num_frames; f++) {
    uint64_t start = f * (uint64_t)H;

    for (uint16_t c = 0; c < channels; c++) {
      // Fill timebuf with windowed samples, zero-pad beyond end.
      for (uint32_t n = 0; n < N; n++) {
        uint64_t idx = start + n;
        double x = 0.0;
        if (idx < total_frames) {
          x = audio_interleaved[idx * channels + c];
        }
        timebuf[n] = x * (double)w[n];
      }

      fftw_execute(plan);

      // Write as complex float32
      for (uint32_t k = 0; k < bins; k++) {
        float re = static_cast<float>(freqbuf.get()[k][0]);
        float im = static_cast<float>(freqbuf.get()[k][1]);
        out.write(reinterpret_cast<const char*>(&re), sizeof(float));
        out.write(reinterpret_cast<const char*>(&im), sizeof(float));
      }
    }
  }

  fftw_destroy_plan(plan);
  out.close();
}

void fafr_decode_fafr_to_wav(const std::string& in_path,
                             const std::string& out_path) {
  std::ifstream in(in_path, std::ios::binary);
  if (!in) throw std::runtime_error("Failed to open input .fafr.");

  FafrHeader hdr{};
  in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
  if (!in) throw std::runtime_error("Failed to read header.");
  if (hdr.magic != FAFR_MAGIC) throw std::runtime_error("Not a FAFR file (bad magic).");
  if (hdr.version != FAFR_VERSION) throw std::runtime_error("Unsupported FAFR version.");
  if (hdr.dtype != 1) throw std::runtime_error("Unsupported dtype (expected complex float32).");

  const uint32_t sr = hdr.sample_rate;
  const uint16_t channels = hdr.channels;
  const uint32_t N = hdr.frame_size;
  const uint32_t H = hdr.hop_size;
  const uint64_t total_frames = hdr.total_samples_per_channel;
  const uint64_t num_frames = hdr.total_frames;
  const uint32_t bins = N / 2 + 1;

  auto w = hann_window(N);

  // Output length from hop coverage: last frame start + N
  uint64_t out_len = 0;
  if (num_frames == 0) {
    out_len = total_frames;
  } else {
    uint64_t last_start = (num_frames - 1) * (uint64_t)H;
    out_len = last_start + N;
    // Trim to original total_frames if you want exact length
    out_len = std::max<uint64_t>(out_len, total_frames);
  }

  std::vector<double> out_accum(out_len * channels, 0.0);
  std::vector<double> win_accum(out_len * channels, 0.0);

  // FFTW inverse
  std::unique_ptr<fftw_complex, decltype(&fftw_free)> freqbuf(
      fftw_alloc_complex(bins), &fftw_free);
  if (!freqbuf) throw std::runtime_error("fftw_alloc_complex failed.");
  std::vector<double> timebuf(N);
  fftw_plan iplan = fftw_plan_dft_c2r_1d((int)N, freqbuf.get(), timebuf.data(), FFTW_ESTIMATE);

  for (uint64_t f = 0; f < num_frames; f++) {
    uint64_t start = f * (uint64_t)H;

    for (uint16_t c = 0; c < channels; c++) {
      // Read bins
      for (uint32_t k = 0; k < bins; k++) {
        float re, im;
        in.read(reinterpret_cast<char*>(&re), sizeof(float));
        in.read(reinterpret_cast<char*>(&im), sizeof(float));
        if (!in) throw std::runtime_error("Unexpected EOF in coefficient data.");
        freqbuf.get()[k][0] = (double)re;
        freqbuf.get()[k][1] = (double)im;
      }

      fftw_execute(iplan);

      // FFTW c2r output is unnormalized: divide by N
      for (uint32_t n = 0; n < N; n++) {
        double x = (timebuf[n] / (double)N) * (double)w[n];
        uint64_t idx = start + n;
        if (idx < out_len) {
          out_accum[idx * channels + c] += x;
          win_accum[idx * channels + c] += (double)w[n] * (double)w[n];
        }
      }
    }
  }

  fftw_destroy_plan(iplan);

  // Normalize overlap-add by window power
  std::vector<float> out_interleaved(total_frames * channels, 0.0f);
  for (uint64_t i = 0; i < total_frames; i++) {
    for (uint16_t c = 0; c < channels; c++) {
      double denom = win_accum[i * channels + c];
      double y = (denom > 1e-12) ? (out_accum[i * channels + c] / denom) : 0.0;
      // Safety clamp
      y = std::max(-1.0, std::min(1.0, y));
      out_interleaved[i * channels + c] = (float)y;
    }
  }

  // Write WAV (float32)
  SF_INFO outinfo{};
  outinfo.channels = channels;
  outinfo.samplerate = (int)sr;
  outinfo.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

  SNDFILE* out = sf_open(out_path.c_str(), SFM_WRITE, &outinfo);
  if (!out) throw std::runtime_error(std::string("sf_open write failed: ") + sf_strerror(nullptr));

  sf_count_t wrote = sf_writef_float(out, out_interleaved.data(), (sf_count_t)total_frames);
  sf_close(out);
  if (wrote != (sf_count_t)total_frames) throw std::runtime_error("Failed to write all frames.");
}
