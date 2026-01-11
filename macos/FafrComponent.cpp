#include <AudioToolbox/AudioToolbox.h>

extern "C" void* FafrComponentFactory(CFAllocatorRef allocator, CFUUIDRef typeID) {
  (void)allocator;
  (void)typeID;
  // Stub factory: returns null until the AudioCodec implementation is wired up.
  return nullptr;
}
