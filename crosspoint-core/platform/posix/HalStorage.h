#pragma once
// Linux/MIPS implementation of the subset of CrossPoint's HalStorage used by ZipFile.
// The CrossPoint ZIP parsing algorithms remain unchanged.
#include <Print.h>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
class HalFile {
  std::FILE* fp_ = nullptr;
 public:
  HalFile() = default;
  explicit HalFile(std::FILE* fp) : fp_(fp) {}
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  HalFile(HalFile&& other) noexcept : fp_(std::exchange(other.fp_, nullptr)) {}
  HalFile& operator=(HalFile&& other) noexcept {
    if (this != &other) { close(); fp_ = std::exchange(other.fp_, nullptr); }
    return *this;
  }
  explicit operator bool() const { return fp_ != nullptr; }
  bool isOpen() const { return fp_ != nullptr; }
  bool close() {
    if (!fp_) return true;
    std::FILE* fp = std::exchange(fp_, nullptr);
    return std::fclose(fp) == 0;
  }
  bool seek(size_t off) { return fp_ && off <= LONG_MAX && std::fseek(fp_, static_cast<long>(off), SEEK_SET) == 0; }
  bool seekCur(int64_t off) {
    return fp_ && off >= LONG_MIN && off <= LONG_MAX &&
           std::fseek(fp_, static_cast<long>(off), SEEK_CUR) == 0;
  }
  size_t position() const {
    if (!fp_) return 0;
    const long at = std::ftell(fp_);
    return at < 0 ? 0 : static_cast<size_t>(at);
  }
  size_t write(const void* data, size_t count) {
    return fp_ ? std::fwrite(data, 1, count, fp_) : 0;
  }
  size_t write(const uint8_t* data, size_t count) {
    return fp_ ? std::fwrite(data, 1, count, fp_) : 0;
  }
  int read() { return fp_ ? std::fgetc(fp_) : -1; }
  int read(void* dst, size_t count) {
    if (!fp_ || (!dst && count)) return -1;
    const size_t n = std::fread(dst, 1, count, fp_);
    return n == 0 && std::ferror(fp_) ? -1 : static_cast<int>(n);
  }
  size_t size() {
    if (!fp_) return 0;
    const long previous = std::ftell(fp_);
    if (previous < 0 || std::fseek(fp_, 0, SEEK_END) != 0) return 0;
    const long end = std::ftell(fp_);
    std::fseek(fp_, previous, SEEK_SET);
    return end < 0 ? 0 : static_cast<size_t>(end);
  }
  int available() const {
    if (!fp_) return 0;
    const long pos = std::ftell(fp_);
    if (pos < 0) return 0;
    if (std::fseek(fp_, 0, SEEK_END)) return 0;
    const long end = std::ftell(fp_);
    std::fseek(fp_, pos, SEEK_SET);
    return end > pos ? static_cast<int>(std::min<long>(end-pos, INT_MAX)) : 0;
  }
};
class HalStorage {
 public:
  static HalStorage& getInstance() { static HalStorage storage; return storage; }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    file = HalFile(std::fopen(path.c_str(), "rb"));
    return file.isOpen();
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    file = HalFile(std::fopen(path.c_str(), "wb"));
    return file.isOpen();
  }
  bool exists(const char* path) {
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;
    std::fclose(fp);
    return true;
  }
  bool remove(const char* path) { return std::remove(path) == 0; }
  bool rename(const char* from, const char* to) { return std::rename(from, to) == 0; }
};
#define Storage HalStorage::getInstance()
