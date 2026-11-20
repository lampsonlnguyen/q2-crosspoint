#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <cassert>
#include <cstdint>
#include <cstdio>

// Host smoke test links the complete original CrossPoint renderer object,
// so undefined font/shaping/graphics dependencies fail at link time.
int main() {
  HalDisplay display;
  display.begin();
  GfxRenderer gfx(display);
  gfx.begin();
  gfx.clearScreen(0xff);
  gfx.drawPixel(12, 13, true);
  auto* pixels=display.getFrameBuffer();
  assert(pixels);
  const unsigned at=13*HalDisplay::DISPLAY_WIDTH_BYTES+12/8;
  const uint8_t mask=static_cast<uint8_t>(0x80u>>(12&7));
  assert((pixels[at] & mask)==0);
  std::puts("CrossPod upstream GfxRenderer linked and painted into Q2 HAL");
  return 0;
}
