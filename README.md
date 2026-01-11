# FAFR (Framewise Audio Fourier Representation)

FAFR is a small CLI tool that converts WAV audio into a piecewise Fourier representation, then reconstructs the audio back to WAV. The codec is based on standard STFT-style framing: each short time frame is windowed and transformed into complex frequency bins. Those bins define a local Fourier series for that frame, and the full signal is reconstructed by overlap-add across frames.

This project is intentionally focused on a function-style representation of audio: instead of storing samples directly, it stores the Fourier coefficients that define per-frame functions over time.

## What it does

- **Encode**: WAV -> `.fafr` (framewise Fourier coefficients)
- **Decode**: `.fafr` -> WAV (overlap-add reconstruction)

## How it works

1. Read interleaved WAV samples using libsndfile.
2. Apply a short fade-in/out so the waveform approaches zero at the ends (reduces boundary discontinuities).
3. Split the signal into overlapping frames of size `N` with hop `H`.
4. Apply a Hann window to each frame.
5. Run a real-to-complex FFT to produce `N/2+1` complex bins per frame and channel.
6. Store these bins in `.fafr` along with metadata (sample rate, channels, frame size, hop size, frame count).
7. For decoding, read the bins, run inverse FFT per frame, overlap-add, and normalize by window power.

## Build

FAFR uses CMake, FFTW3, and libsndfile.

```
mkdir -p build
cmake -S . -B build
cmake --build build
```

## Usage

Top-level commands:

### `encode`
Convert a WAV file into a `.fafr` file.

```
./fafr encode <input.wav> <output.fafr> [--frame N] [--hop H] [--fade-ms M]
```

- `--frame N`: FFT frame size in samples (power of two, default 4096)
- `--hop H`: hop size in samples (default 1024)
- `--fade-ms M`: fade in/out duration in milliseconds (default 20)

Example (high level):
```
./fafr encode song.wav song.fafr
```

### `decode`
Reconstruct a WAV file from a `.fafr` file.

```
./fafr decode <input.fafr> <output.wav>
```

Example (high level):
```
./fafr decode song.fafr song.wav
```

## Notes and constraints

- Input must be a WAV file readable by libsndfile.
- Output WAV is written as 32-bit float.
- Frame size should be a power of two for FFTW efficiency.
- Hop size must be in `(0, frame]`.
- The fade-in/out is a pragmatic boundary fix, not a full signal processing solution.

## File format summary (`.fafr`)

The file begins with a fixed header:

- magic and version
- sample rate and channel count
- frame size and hop size
- total samples per channel
- total frame count

Following the header, the file stores interleaved complex bins per frame and channel:

```
frame 0, channel 0 bins..., channel 1 bins..., frame 1, channel 0 bins..., ...
```

Each complex bin is stored as two float32 values: real then imaginary.
