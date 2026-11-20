#pragma once
// Linux implementation of the small Arduino timing/heap subset used by
// original CrossPoint font decompression and shaping. No ESP32 firmware.
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <sys/sysinfo.h>
inline uint32_t millis() {
  using namespace std::chrono;
  return static_cast<uint32_t>(
    duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
inline uint32_t micros() {
  using namespace std::chrono;
  return static_cast<uint32_t>(
    duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}
struct Q2LinuxHeapInfo {
  uint32_t getFreeHeap() const {
    struct sysinfo info{};
    if (::sysinfo(&info)!=0) return 0;
    const uint64_t free=static_cast<uint64_t>(info.freeram)*info.mem_unit;
    return static_cast<uint32_t>(std::min<uint64_t>(free, 0xffffffffu));
  }
  uint32_t getMaxAllocHeap() const { return getFreeHeap(); }
};
inline const Q2LinuxHeapInfo ESP{};
