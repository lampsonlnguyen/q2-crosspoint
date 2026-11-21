// SPDX-License-Identifier: MIT
// POSIX image decoder for the ORIGINAL CrossPoint PageImage/ImageBlock API.
// Uses the bounded PNG and libjpeg decoders already verified under Q2 MIPS.
// Only the framebuffer/SD-cache backend is adapted; no Q2-native reader code.
#include "Epub/converters/ImageDecoderFactory.h"
#include "PngPreview.hpp"
#include "JpegPreview.hpp"
#include <GfxRenderer.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
bool isJpeg(const std::string& path) {
  const auto dot=path.find_last_of('.');
  if(dot==std::string::npos)return false;
  std::string ext=path.substr(dot);
  std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){
    return static_cast<char>(std::tolower(c));});
  return ext==".jpg"||ext==".jpeg";
}
bool isPng(const std::string& path) {
  const auto dot=path.find_last_of('.');
  if(dot==std::string::npos)return false;
  std::string ext=path.substr(dot);
  std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){
    return static_cast<char>(std::tolower(c));});
  return ext==".png";
}
bool decodeFile(const std::string& path,crosspod::PngPreview& decoded) {
  FILE* file=std::fopen(path.c_str(),"rb");
  if(!file)return false;
  if(std::fseek(file,0,SEEK_END)!=0){std::fclose(file);return false;}
  const long bytes=std::ftell(file);
  if(bytes<16||bytes>4L*1024*1024||std::fseek(file,0,SEEK_SET)!=0){
    std::fclose(file);return false;
  }
  std::vector<uint8_t> content(static_cast<size_t>(bytes));
  const bool read=std::fread(content.data(),1,content.size(),file)==content.size();
  std::fclose(file);
  if(!read)return false;
  return isPng(path)?crosspod::decodePngPreview(content.data(),content.size(),decoded):
         isJpeg(path)?crosspod::decodeJpegPreview(content.data(),content.size(),decoded):false;
}
class PosixCrossPointImageDecoder final : public ImageToFramebufferDecoder {
 public:
  bool getDimensions(const std::string& path,ImageDimensions& dims) const override {
    crosspod::PngPreview image;
    if(!decodeFile(path,image) || image.width>32767 || image.height>32767)return false;
    dims.width=static_cast<int16_t>(image.width);
    dims.height=static_cast<int16_t>(image.height);
    return true;
  }
  bool decodeToFramebuffer(const std::string& path,GfxRenderer& renderer,
                           const RenderConfig& cfg) override {
    crosspod::PngPreview image;
    if(!decodeFile(path,image) || !image.width || !image.height ||
       cfg.maxWidth<=0 || cfg.maxHeight<=0)return false;
    int w=std::min<int>(cfg.maxWidth,image.width);
    int h=std::max(1,static_cast<int>(
      static_cast<int64_t>(image.height)*w/image.width));
    if(cfg.useExactDimensions) {
      w=cfg.maxWidth;h=cfg.maxHeight;
    } else if(h>cfg.maxHeight) {
      h=cfg.maxHeight;
      w=std::max<int>(1,static_cast<int64_t>(image.width)*h/image.height);
    }
    static constexpr unsigned bayer[4][4]={
      {0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    for(int y=0;y<h;++y) {
      const unsigned sy=static_cast<unsigned>(
        static_cast<uint64_t>(y)*image.height/h);
      for(int x=0;x<w;++x) {
        const unsigned sx=static_cast<unsigned>(
          static_cast<uint64_t>(x)*image.width/w);
        const unsigned gray=image.gray[static_cast<size_t>(sy)*image.width+sx];
        const unsigned threshold=bayer[(y+cfg.y)&3][(x+cfg.x)&3]*16u+8u;
        if(gray<threshold)renderer.drawPixel(cfg.x+x,cfg.y+y,true);
      }
    }
    return true;
  }
  const char* getFormatName() const override {return "CrossPod PNG/JPEG";}
};
PosixCrossPointImageDecoder decoder;
}
bool ImageDecoderFactory::isFormatSupported(const std::string& path) {
  return isPng(path)||isJpeg(path);
}
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& path) {
  return isFormatSupported(path)?&decoder:nullptr;
}
void ImageToFramebufferDecoder::yieldDuringDecode(uint32_t& lastYieldMs) {
  (void)lastYieldMs;
}
bool ImageToFramebufferDecoder::validateAndStoreDimensions(
    int64_t width,int64_t height,ImageDimensions& out,const char*) {
  if(width<=0||height<=0||width>32767||height>32767||
     width*height>8388608)return false;
  out.width=static_cast<int16_t>(width);
  out.height=static_cast<int16_t>(height);
  return true;
}
void ImageToFramebufferDecoder::warnUnsupportedFeature(
    const std::string&,const std::string&) {}
