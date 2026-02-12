#include "fafr_lib.h"

#include <cstdint>
#include <string>
#include <iostream>
#include <stdexcept>

struct Args {
  std::string mode;         // "encode", "decode", or "equation"
  std::string in_path;
  std::string out_path;
  uint32_t frame_size = 4096;
  uint32_t hop_size   = 1024;
  float fade_ms = 20.0f;
  uint32_t terms = 32;
};

static void usage() {
  std::cerr <<
    "Usage:\n"
    "  fafr encode <input.wav> <output.fafr> [--frame N] [--hop H] [--fade-ms M]\n"
    "  fafr decode <input.fafr> <output.wav>\n"
    "  fafr equation <input.wav> <output.txt> [--terms K]\n"
    "\n"
    "Defaults: frame=4096 hop=1024 fade-ms=20 terms=32\n";
}

static bool starts_with(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

static Args parse_args(int argc, char** argv) {
  if (argc < 4) {
    usage();
    throw std::runtime_error("Not enough arguments.");
  }
  Args a;
  a.mode = argv[1];
  a.in_path = argv[2];
  a.out_path = argv[3];

  for (int i = 4; i < argc; i++) {
    std::string k = argv[i];
    if (k == "--frame" && i + 1 < argc) {
      a.frame_size = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (k == "--hop" && i + 1 < argc) {
      a.hop_size = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (k == "--fade-ms" && i + 1 < argc) {
      a.fade_ms = std::stof(argv[++i]);
    } else if (k == "--terms" && i + 1 < argc) {
      a.terms = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else {
      usage();
      throw std::runtime_error("Unknown arg: " + k);
    }
  }

  if (a.frame_size < 8 || (a.frame_size & (a.frame_size - 1)) != 0) {
    throw std::runtime_error("--frame must be power of two and >= 8 (for FFTW efficiency).");
  }
  if (a.hop_size == 0 || a.hop_size > a.frame_size) {
    throw std::runtime_error("--hop must be in (0, frame].");
  }
  if (a.terms == 0) {
    throw std::runtime_error("--terms must be >= 1.");
  }
  return a;
}

static void encode_wav_to_fafr(const Args& a) {
  FafrEncodeOptions opt{};
  opt.frame_size = a.frame_size;
  opt.hop_size = a.hop_size;
  opt.fade_ms = a.fade_ms;
  fafr_encode_wav_to_fafr(a.in_path, a.out_path, opt);

  std::cout << "Encoded:\n"
            << "  Input:  " << a.in_path << "\n"
            << "  Output: " << a.out_path << "\n"
            << "  Frame: " << a.frame_size << " Hop: " << a.hop_size << "\n"
            << "  Fade: " << a.fade_ms << " ms\n";
}

static void decode_fafr_to_wav(const Args& a) {
  fafr_decode_fafr_to_wav(a.in_path, a.out_path);

  std::cout << "Decoded:\n"
            << "  Input:  " << a.in_path << "\n"
            << "  Output: " << a.out_path << "\n";
}

static void export_wav_equation(const Args& a) {
  FafrEquationOptions opt{};
  opt.max_terms = a.terms;
  fafr_export_wav_equation(a.in_path, a.out_path, opt);

  std::cout << "Equation exported:\n"
            << "  Input:  " << a.in_path << "\n"
            << "  Output: " << a.out_path << "\n"
            << "  Harmonics: " << a.terms << "\n";
}

int main(int argc, char** argv) {
  try {
    Args a = parse_args(argc, argv);

    if (a.mode == "encode") {
      encode_wav_to_fafr(a);
    } else if (a.mode == "decode") {
      decode_fafr_to_wav(a);
    } else if (a.mode == "equation") {
      export_wav_equation(a);
    } else {
      usage();
      return 2;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
