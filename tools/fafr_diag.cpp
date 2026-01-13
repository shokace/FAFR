#include <AudioToolbox/AudioFile.h>
#include <AudioToolbox/AudioComponent.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstring>
#include <string>

static constexpr AudioFileTypeID kFafrFileType = 'fafr';
static constexpr OSType kFafrManufacturer = 'FAFR';

static std::string status_to_string(OSStatus s) {
  char fourcc[5] = {0, 0, 0, 0, 0};
  fourcc[0] = static_cast<char>((s >> 24) & 0xFF);
  fourcc[1] = static_cast<char>((s >> 16) & 0xFF);
  fourcc[2] = static_cast<char>((s >> 8) & 0xFF);
  fourcc[3] = static_cast<char>(s & 0xFF);
  bool printable = (fourcc[0] >= 32 && fourcc[0] <= 126) &&
                   (fourcc[1] >= 32 && fourcc[1] <= 126) &&
                   (fourcc[2] >= 32 && fourcc[2] <= 126) &&
                   (fourcc[3] >= 32 && fourcc[3] <= 126);
  char buf[64];
  if (printable) {
    std::snprintf(buf, sizeof(buf), "%d ('%s')", (int)s, fourcc);
  } else {
    std::snprintf(buf, sizeof(buf), "%d", (int)s);
  }
  return std::string(buf);
}

static void print_component_info(AudioComponent comp) {
  if (!comp) return;
  CFStringRef name = nullptr;
  if (AudioComponentCopyName(comp, &name) == noErr && name) {
    char buffer[256];
    if (CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
      std::printf("Found component: %s\n", buffer);
    } else {
      std::printf("Found component (name unavailable)\n");
    }
    CFRelease(name);
  } else {
    std::printf("Found component (name unavailable)\n");
  }
}

int main(int argc, char** argv) {
  AudioComponentDescription desc{};
  desc.componentType = 'afil';
  desc.componentSubType = 'fafr';
  desc.componentManufacturer = kFafrManufacturer;
  desc.componentFlags = 0;
  desc.componentFlagsMask = 0;

  AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
  if (!comp) {
    std::printf("No AudioFile component found for type='afil' subtype='fafr' manufacturer='FAFR'\n");
  } else {
    print_component_info(comp);
  }

  if (argc < 2) {
    std::printf("Usage: fafr_diag /path/to/file.fafr\n");
    return 0;
  }

  const char* path = argv[1];
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path), std::strlen(path), false);
  if (!url) {
    std::printf("Failed to create URL for path: %s\n", path);
    return 1;
  }

  AudioFileID file = nullptr;
  OSStatus st = AudioFileOpenURL(url, kAudioFileReadPermission, kFafrFileType, &file);
  if (st != noErr) {
    std::printf("AudioFileOpenURL (type='fafr') failed: %s\n", status_to_string(st).c_str());
  } else {
    std::printf("AudioFileOpenURL (type='fafr') succeeded.\n");
    AudioFileClose(file);
  }

  file = nullptr;
  OSStatus st_auto = AudioFileOpenURL(url, kAudioFileReadPermission, 0, &file);
  CFRelease(url);
  if (st_auto != noErr) {
    std::printf("AudioFileOpenURL (type=0/auto) failed: %s\n", status_to_string(st_auto).c_str());
    return 1;
  }

  std::printf("AudioFileOpenURL (type=0/auto) succeeded.\n");
  AudioFileClose(file);
  return 0;
}
