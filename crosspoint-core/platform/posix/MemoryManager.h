#pragma once
// POSIX memory-availability and cache reclamation bridge for CrossPoint.
// The Q2 has 128MB shared RAM; checking actual MemAvailable is more useful
// than retaining the ESP32 heap-specific API.
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <sys/sysinfo.h>
#include <utility>
#include <vector>
namespace freeink {
class MemoryManager {
 public:
  struct Sink {
    const char* name;
    int priority;
    std::function<std::size_t(std::size_t)> reclaim;
  };
  static MemoryManager& instance() {
    static MemoryManager manager;
    return manager;
  }
  void registerSink(Sink sink) { sinks_.push_back(std::move(sink)); }
  std::size_t freeBytes() const {
    // Linux MemAvailable includes reclaimable page cache. This is intentionally
    // not the same as the process RSS cap, which is measured separately.
    std::FILE* fp=std::fopen("/proc/meminfo","r");
    if (!fp) return 0;
    char name[64]{};
    unsigned long kib=0;
    char unit[16]{};
    while (std::fscanf(fp,"%63s %lu %15s",name,&kib,unit)==3) {
      if (std::string(name)=="MemAvailable:") {
        std::fclose(fp);
        return static_cast<std::size_t>(kib)*1024u;
      }
    }
    std::fclose(fp);
    return 0;
  }
  bool ensureFree(std::size_t required) {
    if (freeBytes()>=required) return true;
    // Caller-owned sinks live for the CrossPod reader process lifetime.
    // Renderer teardown must clear the registry before any new renderer is
    // installed; this is a known lifecycle requirement for device validation.
    for (auto& sink:sinks_) {
      if (sink.reclaim) sink.reclaim(required);
      if (freeBytes()>=required) return true;
    }
    return false;
  }
 private:
  std::vector<Sink> sinks_;
};
}
