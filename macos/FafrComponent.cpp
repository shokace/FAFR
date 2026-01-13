#include <AudioToolbox/AudioCodec.h>
#include <AudioToolbox/AudioComponent.h>
#include <CoreFoundation/CoreFoundation.h>

#include "fafr_lib.h"

#include <algorithm>
#include <cstring>
#include <vector>

static constexpr AudioFormatID kFafrFormatId = 'fafr';

struct FafrCodecState {
  AudioComponentPlugInInterface iface;
  AudioComponentInstance instance = nullptr;
  bool initialized = false;
  bool decoded_ready = false;
  bool header_from_cookie = false;
  FafrHeader header{};
  AudioStreamBasicDescription input_format{};
  AudioStreamBasicDescription output_format{};
  std::vector<uint8_t> input_bytes;
  FafrDecodedAudio decoded;
  uint64_t read_frames = 0;
};

static AudioStreamBasicDescription make_fafr_input_format() {
  AudioStreamBasicDescription asbd{};
  asbd.mFormatID = kFafrFormatId;
  asbd.mFramesPerPacket = 0;
  return asbd;
}

static AudioStreamBasicDescription make_pcm_output_format(uint32_t sample_rate, uint16_t channels) {
  AudioStreamBasicDescription asbd{};
  asbd.mSampleRate = sample_rate;
  asbd.mFormatID = kAudioFormatLinearPCM;
  asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  asbd.mBitsPerChannel = 32;
  asbd.mChannelsPerFrame = channels;
  asbd.mFramesPerPacket = 1;
  asbd.mBytesPerFrame = channels * sizeof(float);
  asbd.mBytesPerPacket = asbd.mBytesPerFrame * asbd.mFramesPerPacket;
  return asbd;
}

static OSStatus FafrCodecOpen(void* self, AudioComponentInstance instance) {
  if (!self) return kAudioCodecUnspecifiedError;
  auto* state = static_cast<FafrCodecState*>(self);
  state->instance = instance;
  return noErr;
}

static OSStatus FafrCodecClose(void* self) {
  if (!self) return kAudioCodecUnspecifiedError;
  auto* state = static_cast<FafrCodecState*>(self);
  delete state;
  return noErr;
}

static OSStatus FafrCodecGetPropertyInfo(void* self, AudioCodecPropertyID inPropertyID,
                                         UInt32* outSize, Boolean* outWritable) {
  (void)self;
  if (outWritable) *outWritable = false;
  switch (inPropertyID) {
    case kAudioCodecPropertySupportedInputFormats:
    case kAudioCodecPropertySupportedOutputFormats:
    case kAudioCodecPropertyCurrentInputFormat:
    case kAudioCodecPropertyCurrentOutputFormat:
      if (outSize) *outSize = sizeof(AudioStreamBasicDescription);
      if (outWritable && (inPropertyID == kAudioCodecPropertyCurrentInputFormat ||
                          inPropertyID == kAudioCodecPropertyCurrentOutputFormat)) {
        *outWritable = true;
      }
      return noErr;
    case kAudioCodecPropertyIsInitialized:
      if (outSize) *outSize = sizeof(UInt32);
      return noErr;
    case kAudioCodecPropertyNameCFString:
    case kAudioCodecPropertyManufacturerCFString:
    case kAudioCodecPropertyFormatCFString:
      if (outSize) *outSize = sizeof(CFStringRef);
      return noErr;
    default:
      return kAudioCodecUnknownPropertyError;
  }
}

static OSStatus FafrCodecGetProperty(void* self, AudioCodecPropertyID inPropertyID,
                                     UInt32* ioPropertyDataSize, void* outPropertyData) {
  if (!self || !ioPropertyDataSize || !outPropertyData) return kAudioCodecUnspecifiedError;
  auto* state = static_cast<FafrCodecState*>(self);

  switch (inPropertyID) {
    case kAudioCodecPropertySupportedInputFormats: {
      if (*ioPropertyDataSize < sizeof(AudioStreamBasicDescription)) return kAudioCodecBadPropertySizeError;
      auto asbd = make_fafr_input_format();
      std::memcpy(outPropertyData, &asbd, sizeof(asbd));
      *ioPropertyDataSize = sizeof(asbd);
      return noErr;
    }
    case kAudioCodecPropertySupportedOutputFormats: {
      if (*ioPropertyDataSize < sizeof(AudioStreamBasicDescription)) return kAudioCodecBadPropertySizeError;
      AudioStreamBasicDescription asbd = make_pcm_output_format(44100, 2);
      std::memcpy(outPropertyData, &asbd, sizeof(asbd));
      *ioPropertyDataSize = sizeof(asbd);
      return noErr;
    }
    case kAudioCodecPropertyCurrentInputFormat: {
      if (*ioPropertyDataSize < sizeof(AudioStreamBasicDescription)) return kAudioCodecBadPropertySizeError;
      std::memcpy(outPropertyData, &state->input_format, sizeof(state->input_format));
      *ioPropertyDataSize = sizeof(state->input_format);
      return noErr;
    }
    case kAudioCodecPropertyCurrentOutputFormat: {
      if (*ioPropertyDataSize < sizeof(AudioStreamBasicDescription)) return kAudioCodecBadPropertySizeError;
      std::memcpy(outPropertyData, &state->output_format, sizeof(state->output_format));
      *ioPropertyDataSize = sizeof(state->output_format);
      return noErr;
    }
    case kAudioCodecPropertyIsInitialized: {
      if (*ioPropertyDataSize < sizeof(UInt32)) return kAudioCodecBadPropertySizeError;
      UInt32 flag = state->initialized ? 1 : 0;
      std::memcpy(outPropertyData, &flag, sizeof(flag));
      *ioPropertyDataSize = sizeof(flag);
      return noErr;
    }
    case kAudioCodecPropertyNameCFString: {
      if (*ioPropertyDataSize < sizeof(CFStringRef)) return kAudioCodecBadPropertySizeError;
      CFStringRef s = CFStringCreateWithCString(nullptr, "FAFR Decoder", kCFStringEncodingUTF8);
      std::memcpy(outPropertyData, &s, sizeof(CFStringRef));
      *ioPropertyDataSize = sizeof(CFStringRef);
      return noErr;
    }
    case kAudioCodecPropertyManufacturerCFString: {
      if (*ioPropertyDataSize < sizeof(CFStringRef)) return kAudioCodecBadPropertySizeError;
      CFStringRef s = CFStringCreateWithCString(nullptr, "FAFR", kCFStringEncodingUTF8);
      std::memcpy(outPropertyData, &s, sizeof(CFStringRef));
      *ioPropertyDataSize = sizeof(CFStringRef);
      return noErr;
    }
    case kAudioCodecPropertyFormatCFString: {
      if (*ioPropertyDataSize < sizeof(CFStringRef)) return kAudioCodecBadPropertySizeError;
      CFStringRef s = CFStringCreateWithCString(nullptr, "FAFR", kCFStringEncodingUTF8);
      std::memcpy(outPropertyData, &s, sizeof(CFStringRef));
      *ioPropertyDataSize = sizeof(CFStringRef);
      return noErr;
    }
    default:
      return kAudioCodecUnknownPropertyError;
  }
}

static OSStatus FafrCodecSetProperty(void* self, AudioCodecPropertyID inPropertyID,
                                     UInt32 inPropertyDataSize, const void* inPropertyData) {
  if (!self || !inPropertyData) return kAudioCodecUnspecifiedError;
  auto* state = static_cast<FafrCodecState*>(self);

  switch (inPropertyID) {
    case kAudioCodecPropertyCurrentInputFormat:
      if (inPropertyDataSize != sizeof(AudioStreamBasicDescription)) return kAudioCodecBadPropertySizeError;
      std::memcpy(&state->input_format, inPropertyData, sizeof(state->input_format));
      return noErr;
    case kAudioCodecPropertyCurrentOutputFormat:
      if (inPropertyDataSize != sizeof(AudioStreamBasicDescription)) return kAudioCodecBadPropertySizeError;
      std::memcpy(&state->output_format, inPropertyData, sizeof(state->output_format));
      return noErr;
    default:
      return kAudioCodecUnknownPropertyError;
  }
}

static OSStatus FafrCodecInitialize(void* self, const AudioStreamBasicDescription* inInputFormat,
                                    const AudioStreamBasicDescription* inOutputFormat,
                                    const void* inMagicCookie, UInt32 inMagicCookieByteSize) {
  if (!self) return kAudioCodecUnspecifiedError;
  auto* state = static_cast<FafrCodecState*>(self);
  state->input_format = inInputFormat ? *inInputFormat : make_fafr_input_format();
  state->output_format = inOutputFormat ? *inOutputFormat : AudioStreamBasicDescription{};
  state->initialized = true;
  state->decoded_ready = false;
  state->header_from_cookie = false;
  state->read_frames = 0;
  state->input_bytes.clear();
  state->decoded = FafrDecodedAudio{};

  if (inMagicCookie && inMagicCookieByteSize >= sizeof(FafrHeader)) {
    FafrHeader hdr{};
    if (fafr_parse_header_bytes(static_cast<const uint8_t*>(inMagicCookie),
                                inMagicCookieByteSize, &hdr)) {
      state->header_from_cookie = true;
      state->header = hdr;
      state->output_format = make_pcm_output_format(hdr.sample_rate, hdr.channels);
    }
  }
  return noErr;
}

static OSStatus FafrCodecUninitialize(void* self) {
  if (!self) return kAudioCodecUnspecifiedError;
  auto* state = static_cast<FafrCodecState*>(self);
  state->initialized = false;
  state->decoded_ready = false;
  state->read_frames = 0;
  state->input_bytes.clear();
  state->decoded = FafrDecodedAudio{};
  return noErr;
}

static OSStatus FafrCodecAppendInputData(void* self, const void* inInputData,
                                         UInt32* ioInputDataByteSize, UInt32* ioNumberPackets,
                                         const AudioStreamPacketDescription* inPacketDescription) {
  (void)inPacketDescription;
  if (!self || !ioInputDataByteSize || !inInputData) return kAudioCodecUnspecifiedError;
  auto* state = static_cast<FafrCodecState*>(self);
  const uint8_t* bytes = static_cast<const uint8_t*>(inInputData);
  const UInt32 size = *ioInputDataByteSize;
  state->input_bytes.insert(state->input_bytes.end(), bytes, bytes + size);
  if (ioNumberPackets) *ioNumberPackets = 1;
  return noErr;
}

static OSStatus FafrCodecProduceOutputPackets(void* self, void* outOutputData,
                                              UInt32* ioOutputDataByteSize, UInt32* ioNumberPackets,
                                              AudioStreamPacketDescription* outPacketDescription,
                                              UInt32* outStatus) {
  (void)outPacketDescription;
  if (!self || !ioOutputDataByteSize || !ioNumberPackets || !outStatus) {
    return kAudioCodecUnspecifiedError;
  }
  auto* state = static_cast<FafrCodecState*>(self);

  if (!state->decoded_ready) {
    if (state->header_from_cookie) {
      const FafrHeader& hdr = state->header;
      uint64_t bins = hdr.frame_size / 2 + 1;
      uint64_t bins_per_frame = bins * hdr.channels;
      uint64_t floats_per_frame = bins_per_frame * 2ULL;
      uint64_t total_floats = floats_per_frame * hdr.total_frames;
      uint64_t coeff_bytes = total_floats * sizeof(float);
      if (state->input_bytes.size() < coeff_bytes) {
        *outStatus = kAudioCodecProduceOutputPacketNeedsMoreInputData;
        *ioNumberPackets = 0;
        return noErr;
      }
      state->decoded = fafr_decode_fafr_coeffs(hdr,
                                               state->input_bytes.data(),
                                               coeff_bytes);
      state->decoded_ready = true;
      state->read_frames = 0;
    } else {
      FafrHeader hdr{};
      if (!fafr_parse_header_bytes(state->input_bytes.data(),
                                   state->input_bytes.size(),
                                   &hdr)) {
        *outStatus = kAudioCodecProduceOutputPacketNeedsMoreInputData;
        *ioNumberPackets = 0;
        return noErr;
      }

      uint64_t bins = hdr.frame_size / 2 + 1;
      uint64_t bins_per_frame = bins * hdr.channels;
      uint64_t floats_per_frame = bins_per_frame * 2ULL;
      uint64_t total_floats = floats_per_frame * hdr.total_frames;
      uint64_t coeff_bytes = total_floats * sizeof(float);
      uint64_t needed = sizeof(FafrHeader) + coeff_bytes;
      if (state->input_bytes.size() < needed) {
        *outStatus = kAudioCodecProduceOutputPacketNeedsMoreInputData;
        *ioNumberPackets = 0;
        return noErr;
      }

      state->decoded = fafr_decode_fafr_bytes(state->input_bytes.data(), needed);
      state->decoded_ready = true;
      state->read_frames = 0;
      state->output_format = make_pcm_output_format(state->decoded.sample_rate,
                                                    state->decoded.channels);
    }
  }

  if (!state->decoded_ready || state->decoded.channels == 0) {
    *outStatus = kAudioCodecProduceOutputPacketFailure;
    *ioNumberPackets = 0;
    return kAudioCodecBadDataError;
  }

  const uint32_t channels = state->decoded.channels;
  const uint32_t bytes_per_frame = channels * sizeof(float);
  const uint64_t frames_available = state->decoded.total_frames - state->read_frames;
  const uint64_t max_frames = *ioOutputDataByteSize / bytes_per_frame;
  const uint64_t frames_to_copy = std::min<uint64_t>(frames_available, max_frames);

  if (frames_to_copy == 0) {
    *outStatus = kAudioCodecProduceOutputPacketNeedsMoreInputData;
    *ioNumberPackets = 0;
    return noErr;
  }

  const float* src = state->decoded.interleaved.data() + state->read_frames * channels;
  std::memcpy(outOutputData, src, frames_to_copy * bytes_per_frame);
  state->read_frames += frames_to_copy;
  *ioNumberPackets = static_cast<UInt32>(frames_to_copy);
  *ioOutputDataByteSize = static_cast<UInt32>(frames_to_copy * bytes_per_frame);
  *outStatus = (state->read_frames >= state->decoded.total_frames)
                 ? kAudioCodecProduceOutputPacketAtEOF
                 : kAudioCodecProduceOutputPacketSuccessHasMore;
  return noErr;
}

static OSStatus FafrCodecReset(void* self) {
  if (!self) return kAudioCodecUnspecifiedError;
  auto* state = static_cast<FafrCodecState*>(self);
  state->decoded_ready = false;
  state->header_from_cookie = false;
  state->read_frames = 0;
  state->input_bytes.clear();
  state->decoded = FafrDecodedAudio{};
  return noErr;
}

static AudioComponentMethod FafrCodecLookup(SInt16 selector) {
  switch (selector) {
    case kAudioCodecGetPropertyInfoSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrCodecGetPropertyInfo);
    case kAudioCodecGetPropertySelect:
      return reinterpret_cast<AudioComponentMethod>(FafrCodecGetProperty);
    case kAudioCodecSetPropertySelect:
      return reinterpret_cast<AudioComponentMethod>(FafrCodecSetProperty);
    case kAudioCodecInitializeSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrCodecInitialize);
    case kAudioCodecUninitializeSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrCodecUninitialize);
    case kAudioCodecAppendInputDataSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrCodecAppendInputData);
    case kAudioCodecProduceOutputDataSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrCodecProduceOutputPackets);
    case kAudioCodecResetSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrCodecReset);
    default:
      return nullptr;
  }
}

extern "C" AudioComponentPlugInInterface* FafrComponentFactory(const AudioComponentDescription* inDesc) {
  (void)inDesc;
  auto* state = new FafrCodecState();
  state->iface.Open = FafrCodecOpen;
  state->iface.Close = FafrCodecClose;
  state->iface.Lookup = FafrCodecLookup;
  state->iface.reserved = nullptr;
  state->input_format = make_fafr_input_format();
  return &state->iface;
}
