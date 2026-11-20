// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
// Linux/Q2 CrossPoint renderer HAL. A 375x320 1-bpp logical canvas is
// presented through Q2PagePresenter; *no* e-ink or ESP32 hardware API.
class HalDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH=375;
  static constexpr uint16_t DISPLAY_HEIGHT=320;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES=(DISPLAY_WIDTH+7)/8;
  static constexpr uint32_t BUFFER_SIZE=DISPLAY_WIDTH_BYTES*DISPLAY_HEIGHT;
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };
  enum class GrayscaleMode { Overlay, Native };
  enum class GrayscaleBase { White, Black };
  enum class GrayscaleEncoding { Differential, Absolute };
  struct GrayscaleCapabilities {
    bool asyncBase=false;
    bool stripUploads=false;
    bool retainedOverlay=false;
    bool baseFrame=false;
    bool available=false;
  };
  HalDisplay();
  ~HalDisplay();
  void begin(bool seamless=false);
  void clearScreen(uint8_t color=0xFF) const;
  void drawImage(const uint8_t* bytes,uint16_t x,uint16_t y,uint16_t w,uint16_t h,bool fromProgmem=false) const;
  void displayBuffer(RefreshMode mode=FAST_REFRESH,bool turnOffScreen=false);
  void displayBufferAsync(RefreshMode mode=FAST_REFRESH) {displayBuffer(mode);}
  void waitRefreshComplete() {}
  bool supportsAsyncRefresh() const {return false;}
  void setInverted(bool inverted) {inverted_=inverted;}
  bool isInverted() const {return inverted_;}
  void deepSleep() {}
  void controllerIdle() {}
  uint8_t* getFrameBuffer() const {return raster_.get();}
  uint8_t* lendFrameBufferStorage(uint32_t* sizeOut);
  void returnFrameBufferStorage();
  void preconditionGrayscale() {}
  void preconditionGrayscale(uint16_t,uint16_t,uint16_t,uint16_t) {}
  bool displayGrayscaleBase(GrayscaleMode,RefreshMode=HALF_REFRESH,bool=false) {return false;}
  void displayGrayscaleBase(RefreshMode=HALF_REFRESH,bool=false) {}
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void cleanupGrayscaleBuffers(const uint8_t*) {}
  void displayGrayBuffer(bool=false) {}
  bool displayRetainedGrayscaleOverlay(uint16_t,uint16_t,uint16_t,uint16_t,bool) {return false;}
  void writeGrayscalePlaneStrip(bool,const uint8_t*,uint16_t,uint16_t) {}
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode=GrayscaleMode::Overlay) const {return {};}
  uint16_t getDisplayWidth() const {return DISPLAY_WIDTH;}
  uint16_t getDisplayHeight() const {return DISPLAY_HEIGHT;}
  uint16_t getDisplayWidthBytes() const {return DISPLAY_WIDTH_BYTES;}
  uint32_t getBufferSize() const {return BUFFER_SIZE;}
 private:
  mutable std::unique_ptr<uint8_t[]> raster_;
  std::unique_ptr<uint8_t[]> inkRaster_;
  bool inverted_=false;
  bool loaned_=false;
};
