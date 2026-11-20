#pragma once
// POSIX/Linux replacement for ESP32 uzlib one-shot InflateReader.
// CrossPoint's FontDecompressor uses init(false), setSource() and read().
// zlib's raw-DEFLATE mode works with original CrossPoint font group encoding.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <limits>
#include <zlib.h>
class InflateReader {
 public:
  static constexpr size_t RING_BYTES = 32768;
  static constexpr size_t RING_SEGMENT_BYTES = 8192;
  static constexpr size_t RING_SEGMENTS = 4;
  ~InflateReader(){deinit();}
  InflateReader()=default;
  InflateReader(const InflateReader&)=delete;
  InflateReader& operator=(const InflateReader&)=delete;
  bool init(bool streaming=false){
    deinit();
    if(streaming)return false; // FontDecompressor uses one-shot only.
    ready_=true;
    return true;
  }
  bool initWithSegments(uint8_t* const (&)[RING_SEGMENTS]) {return false;}
  void deinit(){source_=nullptr; length_=0; ready_=false;}
  void setSource(const uint8_t* input,size_t length){source_=input;length_=length;}
  bool read(uint8_t* dest,size_t bytes){
    if(!ready_ || !source_ || !dest ||
       bytes>std::numeric_limits<uInt>::max() ||
       length_>std::numeric_limits<uInt>::max())return false;
    z_stream stream{};
    stream.next_in=const_cast<Bytef*>(source_);
    stream.avail_in=static_cast<uInt>(length_);
    stream.next_out=dest;
    stream.avail_out=static_cast<uInt>(bytes);
    if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK)return false;
    const int result=inflate(&stream,Z_FINISH);
    const bool ok=result==Z_STREAM_END && stream.total_out==bytes;
    inflateEnd(&stream);
    return ok;
  }
 private:
  const uint8_t* source_=nullptr;
  size_t length_=0;
  bool ready_=false;
};
