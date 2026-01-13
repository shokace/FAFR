#include <AudioToolbox/AudioFileComponent.h>
#include <AudioToolbox/AudioComponent.h>
#include <CoreFoundation/CoreFoundation.h>

#include "fafr_lib.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

static constexpr AudioFormatID kFafrFormatId = 'fafr';
static constexpr AudioFileTypeID kFafrFileType = 'fafr';
static constexpr UInt32 kFafrPacketSize = 4096;

struct FafrAudioFileState {
  AudioComponentPlugInInterface iface;
  AudioComponentInstance instance = nullptr;
  std::vector<uint8_t> file_bytes;
  FafrHeader header{};
  bool header_valid = false;
};

static bool load_file_from_url(CFURLRef url, std::vector<uint8_t>* out_bytes) {
  if (!url || !out_bytes) return false;
  char path[PATH_MAX] = {0};
  CFURLRef abs_url = CFURLCopyAbsoluteURL(url);
  CFURLRef use_url = abs_url ? abs_url : url;
  bool got_path = CFURLGetFileSystemRepresentation(use_url, true,
                                                   reinterpret_cast<UInt8*>(path),
                                                   sizeof(path));
  if (!got_path) {
    CFStringRef cf_path = CFURLCopyFileSystemPath(use_url, kCFURLPOSIXPathStyle);
    if (cf_path) {
      got_path = CFStringGetCString(cf_path, path, sizeof(path), kCFStringEncodingUTF8);
      CFRelease(cf_path);
    }
  }
  if (abs_url) CFRelease(abs_url);
  if (!got_path || path[0] == '\0') return false;

  if (path[0] != '/') {
    char cwd[PATH_MAX] = {0};
    if (getcwd(cwd, sizeof(cwd))) {
      std::string abs = std::string(cwd) + "/" + std::string(path);
      std::strncpy(path, abs.c_str(), sizeof(path) - 1);
      path[sizeof(path) - 1] = '\0';
    }
  }
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  std::streamsize size = in.tellg();
  if (size <= 0) return false;
  in.seekg(0, std::ios::beg);
  out_bytes->assign(static_cast<size_t>(size), 0);
  return in.read(reinterpret_cast<char*>(out_bytes->data()), size).good();
}

static bool load_file_from_callbacks(void* client,
                                     AudioFile_ReadProc read_proc,
                                     AudioFile_GetSizeProc get_size_proc,
                                     std::vector<uint8_t>* out_bytes) {
  if (!read_proc || !get_size_proc || !out_bytes) return false;
  SInt64 size = get_size_proc(client);
  if (size <= 0) return false;
  out_bytes->assign(static_cast<size_t>(size), 0);
  UInt32 actual = 0;
  if (read_proc(client, 0, static_cast<UInt32>(size), out_bytes->data(), &actual) != noErr) {
    return false;
  }
  out_bytes->resize(actual);
  return actual == static_cast<UInt32>(size);
}

static bool load_file_from_fd(int fd, std::vector<uint8_t>* out_bytes) {
  if (!out_bytes || fd < 0) return false;
  struct stat st {};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) return false;
  out_bytes->assign(static_cast<size_t>(st.st_size), 0);
  size_t offset = 0;
  while (offset < out_bytes->size()) {
    ssize_t got = pread(fd, out_bytes->data() + offset,
                        out_bytes->size() - offset, static_cast<off_t>(offset));
    if (got <= 0) return false;
    offset += static_cast<size_t>(got);
  }
  return true;
}

static OSStatus FafrAudioFileOpen(void* self, AudioComponentInstance instance) {
  if (!self) return kAudioFileUnspecifiedError;
  auto* state = static_cast<FafrAudioFileState*>(self);
  state->instance = instance;
  return noErr;
}

static OSStatus FafrAudioFileClose(void* self) {
  if (!self) return kAudioFileUnspecifiedError;
  auto* state = static_cast<FafrAudioFileState*>(self);
  // Avoid premature deletion; AudioComponent may retain the interface pointer after Close.
  state->file_bytes.clear();
  state->header_valid = false;
  return noErr;
}

static OSStatus FafrAudioFileOpenURL(void* self, CFURLRef inFileRef, SInt8 inPermissions, int inFileDescriptor) {
  (void)inPermissions;
  (void)inFileDescriptor;
  if (!self) return kAudioFileUnspecifiedError;
  auto* state = static_cast<FafrAudioFileState*>(self);
  state->file_bytes.clear();
  state->header_valid = false;

  if (inFileDescriptor >= 0) {
    if (!load_file_from_fd(inFileDescriptor, &state->file_bytes)) {
      return kAudioFileUnspecifiedError;
    }
  } else {
    if (!load_file_from_url(inFileRef, &state->file_bytes)) {
      return kAudioFileUnspecifiedError;
    }
  }
  state->header_valid = fafr_parse_header_bytes(state->file_bytes.data(),
                                                state->file_bytes.size(),
                                                &state->header);
  return state->header_valid ? noErr : kAudioFileUnsupportedDataFormatError;
}

static OSStatus FafrAudioFileOpenWithCallbacks(void* self, void* inClientData,
                                               AudioFile_ReadProc inReadFunc,
                                               AudioFile_WriteProc inWriteFunc,
                                               AudioFile_GetSizeProc inGetSizeFunc,
                                               AudioFile_SetSizeProc inSetSizeFunc) {
  (void)inWriteFunc;
  (void)inSetSizeFunc;
  if (!self) return kAudioFileUnspecifiedError;
  auto* state = static_cast<FafrAudioFileState*>(self);
  state->file_bytes.clear();
  state->header_valid = false;

  if (!load_file_from_callbacks(inClientData, inReadFunc, inGetSizeFunc, &state->file_bytes)) {
    return kAudioFileUnspecifiedError;
  }
  state->header_valid = fafr_parse_header_bytes(state->file_bytes.data(),
                                                state->file_bytes.size(),
                                                &state->header);
  return state->header_valid ? noErr : kAudioFileUnsupportedDataFormatError;
}

static OSStatus FafrAudioFileReadBytes(void* self, Boolean inUseCache, SInt64 inStartingByte,
                                       UInt32* ioNumBytes, void* outBuffer) {
  (void)inUseCache;
  if (!self || !ioNumBytes || !outBuffer) return kAudioFileUnspecifiedError;
  auto* state = static_cast<FafrAudioFileState*>(self);
  if (inStartingByte < 0) return kAudioFileUnspecifiedError;

  const uint64_t size = state->file_bytes.size();
  if (static_cast<uint64_t>(inStartingByte) >= size) {
    *ioNumBytes = 0;
    return kAudioFileEndOfFileError;
  }

  uint64_t available = size - static_cast<uint64_t>(inStartingByte);
  uint32_t to_read = std::min<uint64_t>(available, *ioNumBytes);
  std::memcpy(outBuffer, state->file_bytes.data() + inStartingByte, to_read);
  *ioNumBytes = to_read;
  return (to_read < available) ? noErr : kAudioFileEndOfFileError;
}

static OSStatus FafrAudioFileReadPackets(void* self, Boolean inUseCache, UInt32* outNumBytes,
                                         AudioStreamPacketDescription* outPacketDescriptions,
                                         SInt64 inStartingPacket, UInt32* ioNumPackets, void* outBuffer) {
  (void)inUseCache;
  if (!self || !outNumBytes || !ioNumPackets || !outBuffer) return kAudioFileUnspecifiedError;
  auto* state = static_cast<FafrAudioFileState*>(self);
  if (!state->header_valid) return kAudioFileUnsupportedDataFormatError;
  if (inStartingPacket < 0) return kAudioFileUnspecifiedError;

  const uint64_t data_offset = sizeof(FafrHeader);
  if (state->file_bytes.size() < data_offset) return kAudioFileUnsupportedDataFormatError;
  const uint64_t data_bytes = state->file_bytes.size() - data_offset;

  const uint64_t start_byte = data_offset + static_cast<uint64_t>(inStartingPacket) * kFafrPacketSize;
  if (start_byte >= state->file_bytes.size()) {
    *ioNumPackets = 0;
    *outNumBytes = 0;
    return kAudioFileEndOfFileError;
  }

  uint64_t max_packets = *ioNumPackets;
  uint64_t max_bytes = max_packets * kFafrPacketSize;
  uint64_t available = state->file_bytes.size() - start_byte;
  uint64_t to_read = std::min<uint64_t>(available, max_bytes);

  std::memcpy(outBuffer, state->file_bytes.data() + start_byte, to_read);
  *outNumBytes = static_cast<UInt32>(to_read);

  uint64_t packets_read = (to_read + kFafrPacketSize - 1) / kFafrPacketSize;
  *ioNumPackets = static_cast<UInt32>(packets_read);

  if (outPacketDescriptions) {
    for (uint64_t i = 0; i < packets_read; i++) {
      uint64_t offset = i * kFafrPacketSize;
      uint64_t remaining = to_read - offset;
      uint32_t packet_bytes = static_cast<uint32_t>(std::min<uint64_t>(remaining, kFafrPacketSize));
      outPacketDescriptions[i].mStartOffset = offset;
      outPacketDescriptions[i].mDataByteSize = packet_bytes;
      outPacketDescriptions[i].mVariableFramesInPacket = 0;
    }
  }

  return (start_byte + to_read >= data_offset + data_bytes) ? kAudioFileEndOfFileError : noErr;
}

static OSStatus FafrAudioFileGetPropertyInfo(void* self, AudioFileComponentPropertyID inPropertyID,
                                             UInt32* outDataSize, Boolean* outWritable) {
  (void)self;
  if (outWritable) *outWritable = false;
  switch (inPropertyID) {
    case kAudioFilePropertyFileFormat:
      if (outDataSize) *outDataSize = sizeof(AudioFileTypeID);
      return noErr;
    case kAudioFilePropertyDataFormat:
      if (outDataSize) *outDataSize = sizeof(AudioStreamBasicDescription);
      return noErr;
    case kAudioFilePropertyAudioDataByteCount:
    case kAudioFilePropertyAudioDataPacketCount:
      if (outDataSize) *outDataSize = sizeof(UInt64);
      return noErr;
    case kAudioFilePropertyMaximumPacketSize:
      if (outDataSize) *outDataSize = sizeof(UInt32);
      return noErr;
    case kAudioFilePropertyDataOffset:
      if (outDataSize) *outDataSize = sizeof(SInt64);
      return noErr;
    case kAudioFilePropertyIsOptimized:
      if (outDataSize) *outDataSize = sizeof(UInt32);
      return noErr;
    case kAudioFilePropertyMagicCookieData:
      if (outDataSize) *outDataSize = sizeof(FafrHeader);
      return noErr;
    default:
      return kAudioFileUnsupportedPropertyError;
  }
}

static OSStatus FafrAudioFileGetProperty(void* self, AudioFileComponentPropertyID inPropertyID,
                                         UInt32* ioDataSize, void* outPropertyData) {
  if (!self || !ioDataSize || !outPropertyData) return kAudioFileUnspecifiedError;
  auto* state = static_cast<FafrAudioFileState*>(self);
  if (!state->header_valid) return kAudioFileUnsupportedDataFormatError;

  switch (inPropertyID) {
    case kAudioFilePropertyFileFormat: {
      if (*ioDataSize < sizeof(AudioFileTypeID)) return kAudioFileBadPropertySizeError;
      AudioFileTypeID ft = kFafrFileType;
      std::memcpy(outPropertyData, &ft, sizeof(ft));
      *ioDataSize = sizeof(ft);
      return noErr;
    }
    case kAudioFilePropertyDataFormat: {
      if (*ioDataSize < sizeof(AudioStreamBasicDescription)) return kAudioFileBadPropertySizeError;
      AudioStreamBasicDescription asbd{};
      asbd.mFormatID = kFafrFormatId;
      asbd.mSampleRate = state->header.sample_rate;
      asbd.mChannelsPerFrame = state->header.channels;
      std::memcpy(outPropertyData, &asbd, sizeof(asbd));
      *ioDataSize = sizeof(asbd);
      return noErr;
    }
    case kAudioFilePropertyAudioDataByteCount: {
      if (*ioDataSize < sizeof(UInt64)) return kAudioFileBadPropertySizeError;
      UInt64 bytes = 0;
      if (state->file_bytes.size() > sizeof(FafrHeader)) {
        bytes = static_cast<UInt64>(state->file_bytes.size() - sizeof(FafrHeader));
      }
      std::memcpy(outPropertyData, &bytes, sizeof(bytes));
      *ioDataSize = sizeof(bytes);
      return noErr;
    }
    case kAudioFilePropertyAudioDataPacketCount: {
      if (*ioDataSize < sizeof(UInt64)) return kAudioFileBadPropertySizeError;
      UInt64 data_bytes = 0;
      if (state->file_bytes.size() > sizeof(FafrHeader)) {
        data_bytes = static_cast<UInt64>(state->file_bytes.size() - sizeof(FafrHeader));
      }
      UInt64 packets = (data_bytes + kFafrPacketSize - 1) / kFafrPacketSize;
      std::memcpy(outPropertyData, &packets, sizeof(packets));
      *ioDataSize = sizeof(packets);
      return noErr;
    }
    case kAudioFilePropertyMaximumPacketSize: {
      if (*ioDataSize < sizeof(UInt32)) return kAudioFileBadPropertySizeError;
      UInt32 ps = kFafrPacketSize;
      std::memcpy(outPropertyData, &ps, sizeof(ps));
      *ioDataSize = sizeof(ps);
      return noErr;
    }
    case kAudioFilePropertyDataOffset: {
      if (*ioDataSize < sizeof(SInt64)) return kAudioFileBadPropertySizeError;
      SInt64 offset = static_cast<SInt64>(sizeof(FafrHeader));
      std::memcpy(outPropertyData, &offset, sizeof(offset));
      *ioDataSize = sizeof(offset);
      return noErr;
    }
    case kAudioFilePropertyIsOptimized: {
      if (*ioDataSize < sizeof(UInt32)) return kAudioFileBadPropertySizeError;
      UInt32 opt = 1;
      std::memcpy(outPropertyData, &opt, sizeof(opt));
      *ioDataSize = sizeof(opt);
      return noErr;
    }
    case kAudioFilePropertyMagicCookieData: {
      if (*ioDataSize < sizeof(FafrHeader)) return kAudioFileBadPropertySizeError;
      std::memcpy(outPropertyData, &state->header, sizeof(FafrHeader));
      *ioDataSize = sizeof(FafrHeader);
      return noErr;
    }
    default:
      return kAudioFileUnsupportedPropertyError;
  }
}

static OSStatus FafrAudioFileExtensionIsThisFormat(void* self, CFStringRef inExtension, UInt32* outResult) {
  (void)self;
  if (!outResult) return kAudioFileUnspecifiedError;
  *outResult = 0;
  if (!inExtension) return noErr;
  if (CFStringCompare(inExtension, CFSTR("fafr"), kCFCompareCaseInsensitive) == kCFCompareEqualTo) {
    *outResult = 1;
  }
  return noErr;
}

static OSStatus FafrAudioFileFileDataIsThisFormat(void* self, UInt32 inDataByteSize,
                                                  const void* inData, UInt32* outResult) {
  (void)self;
  if (!outResult || !inData) return kAudioFileUnspecifiedError;
  *outResult = 0;
  if (inDataByteSize >= sizeof(uint32_t)) {
    uint32_t magic = 0;
    std::memcpy(&magic, inData, sizeof(uint32_t));
    if (magic == FAFR_MAGIC) {
      *outResult = 1;
      return noErr;
    }
  }
  FafrHeader hdr{};
  if (fafr_parse_header_bytes(static_cast<const uint8_t*>(inData), inDataByteSize, &hdr)) {
    *outResult = 1;
  }
  return noErr;
}

static OSStatus FafrAudioFileGetGlobalInfoSize(void* self, AudioFileComponentPropertyID inPropertyID,
                                               UInt32 inSpecifierSize, const void* inSpecifier,
                                               UInt32* ioPropertyDataSize) {
  (void)self;
  (void)inSpecifierSize;
  (void)inSpecifier;
  if (!ioPropertyDataSize) return kAudioFileUnspecifiedError;
  switch (inPropertyID) {
    case kAudioFileComponent_CanRead:
    case kAudioFileComponent_CanWrite:
      *ioPropertyDataSize = sizeof(UInt32);
      return noErr;
    case kAudioFileComponent_FileTypeName:
      *ioPropertyDataSize = sizeof(CFStringRef);
      return noErr;
    case kAudioFileComponent_ExtensionsForType:
    case kAudioFileComponent_UTIsForType:
      *ioPropertyDataSize = sizeof(CFArrayRef);
      return noErr;
    case kAudioFileComponent_AvailableFormatIDs:
      *ioPropertyDataSize = sizeof(AudioFormatID);
      return noErr;
    default:
      return kAudioFileUnsupportedPropertyError;
  }
}

static OSStatus FafrAudioFileGetGlobalInfo(void* self, AudioFileComponentPropertyID inPropertyID,
                                           UInt32 inSpecifierSize, const void* inSpecifier,
                                           UInt32* ioPropertyDataSize, void* outPropertyData) {
  (void)self;
  (void)inSpecifierSize;
  (void)inSpecifier;
  if (!ioPropertyDataSize || !outPropertyData) return kAudioFileUnspecifiedError;
  switch (inPropertyID) {
    case kAudioFileComponent_CanRead: {
      if (*ioPropertyDataSize < sizeof(UInt32)) return kAudioFileBadPropertySizeError;
      UInt32 v = 1;
      std::memcpy(outPropertyData, &v, sizeof(v));
      *ioPropertyDataSize = sizeof(v);
      return noErr;
    }
    case kAudioFileComponent_CanWrite: {
      if (*ioPropertyDataSize < sizeof(UInt32)) return kAudioFileBadPropertySizeError;
      UInt32 v = 0;
      std::memcpy(outPropertyData, &v, sizeof(v));
      *ioPropertyDataSize = sizeof(v);
      return noErr;
    }
    case kAudioFileComponent_FileTypeName: {
      if (*ioPropertyDataSize < sizeof(CFStringRef)) return kAudioFileBadPropertySizeError;
      CFStringRef s = CFStringCreateWithCString(nullptr, "FAFR", kCFStringEncodingUTF8);
      std::memcpy(outPropertyData, &s, sizeof(s));
      *ioPropertyDataSize = sizeof(s);
      return noErr;
    }
    case kAudioFileComponent_ExtensionsForType: {
      if (*ioPropertyDataSize < sizeof(CFArrayRef)) return kAudioFileBadPropertySizeError;
      const void* values[] = { CFSTR("fafr") };
      CFArrayRef arr = CFArrayCreate(nullptr, values, 1, &kCFTypeArrayCallBacks);
      std::memcpy(outPropertyData, &arr, sizeof(arr));
      *ioPropertyDataSize = sizeof(arr);
      return noErr;
    }
    case kAudioFileComponent_UTIsForType: {
      if (*ioPropertyDataSize < sizeof(CFArrayRef)) return kAudioFileBadPropertySizeError;
      const void* values[] = { CFSTR("com.fafr.audio") };
      CFArrayRef arr = CFArrayCreate(nullptr, values, 1, &kCFTypeArrayCallBacks);
      std::memcpy(outPropertyData, &arr, sizeof(arr));
      *ioPropertyDataSize = sizeof(arr);
      return noErr;
    }
    case kAudioFileComponent_AvailableFormatIDs: {
      if (*ioPropertyDataSize < sizeof(AudioFormatID)) return kAudioFileBadPropertySizeError;
      AudioFormatID fmt = kFafrFormatId;
      std::memcpy(outPropertyData, &fmt, sizeof(fmt));
      *ioPropertyDataSize = sizeof(fmt);
      return noErr;
    }
    default:
      return kAudioFileUnsupportedPropertyError;
  }
}

static AudioComponentMethod FafrAudioFileLookup(SInt16 selector) {
  switch (selector) {
    case kAudioFileOpenURLSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileOpenURL);
    case kAudioFileOpenWithCallbacksSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileOpenWithCallbacks);
    case kAudioFileCloseSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileClose);
    case kAudioFileReadBytesSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileReadBytes);
    case kAudioFileReadPacketsSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileReadPackets);
    case kAudioFileReadPacketDataSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileReadPackets);
    case kAudioFileGetPropertyInfoSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileGetPropertyInfo);
    case kAudioFileGetPropertySelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileGetProperty);
    case kAudioFileExtensionIsThisFormatSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileExtensionIsThisFormat);
    case kAudioFileFileDataIsThisFormatSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileFileDataIsThisFormat);
    case kAudioFileGetGlobalInfoSizeSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileGetGlobalInfoSize);
    case kAudioFileGetGlobalInfoSelect:
      return reinterpret_cast<AudioComponentMethod>(FafrAudioFileGetGlobalInfo);
    default:
      return nullptr;
  }
}

extern "C" AudioComponentPlugInInterface* FafrAudioFileComponentFactory(const AudioComponentDescription* inDesc) {
  (void)inDesc;
  auto* state = new FafrAudioFileState();
  state->iface.Open = FafrAudioFileOpen;
  state->iface.Close = FafrAudioFileClose;
  state->iface.Lookup = FafrAudioFileLookup;
  state->iface.reserved = nullptr;
  return &state->iface;
}
