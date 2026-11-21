// SPDX-License-Identifier: MIT
// Runtime gate for UNMODIFIED CrossPoint ChapterHtmlSlimParser + Section.
// Unlike an interface or linker test, this builds a real EPUB section and
// paints a Page through GfxRenderer using Q2's HAL framebuffer.
#include <Epub.h>
#include <Section.h>
#include <Page.h>
#include <ReaderRenderSpec.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <builtinFonts/notosans_12_regular.h>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>

int main(int argc,char** argv) {
  if(argc!=2)return 2;
  char temp[]="/tmp/crosspod-original-engine-XXXXXX";
  char* dir=mkdtemp(temp);
  assert(dir);
  auto epub=std::make_shared<Epub>(std::string(argv[1]),std::string(dir));
  assert(epub->load(true,false));
  assert(epub->getSpineItemsCount()>0);
  HalDisplay display;
  display.begin();
  GfxRenderer gfx(display);
  gfx.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  gfx.begin();
  EpdFont body(&notosans_12_regular);
  gfx.insertFont(1,EpdFontFamily(12,&body));
  FontDecompressor decompressor;
  assert(decompressor.init());
  FontCacheManager cache(gfx.getFontMap(),gfx.getSdCardFonts(),gfx.getTtfFonts());
  cache.setFontDecompressor(&decompressor);
  gfx.setFontCacheManager(&cache);
  ReaderRenderSpec spec;
  spec.fontId=1;
  spec.viewportWidth=345;
  spec.viewportHeight=280;
  spec.embeddedStyle=true;
  spec.imageRendering=1; // First runtime link gate tests text+CSS; images separately.
  spec.hyphenationEnabled=false;
  Section section(epub,0,gfx);
  assert(section.createSectionFile(spec));
  assert(section.pageCount>0);
  std::unique_ptr<Page> page=section.loadPage(0);
  assert(page && !page->elements.empty());
  gfx.clearScreen(0xff);
  page->render(gfx,1,14,16);
  const uint8_t* framebuffer=display.getFrameBuffer();
  assert(framebuffer);
  size_t ink=0;
  for(size_t i=0;i<display.getBufferSize();++i) {
    const auto b=framebuffer[i];
    for(int bit=0;bit<8;++bit)ink+=((b>>bit)&1)==0;
  }
  assert(ink>80);
  std::printf("Original CrossPoint ChapterHtmlSlimParser and Section: %u page(s), %zu black pixels\n",
      section.pageCount,ink);
  return 0;
}
