// SPDX-License-Identifier: MIT
#pragma once
// CrossPoint's own cpfont-v4 SD fonts. Only the Unicode coverage indexes and
// glyphs actually needed for a page are resident; never expand the full bitmap
// files into the Shanling Q2's 128 MB RAM.
#include "Typography.hpp"
#include <GfxRenderer.h>
#include <SdCardFont.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace crosspod {
struct ScriptFontSpec {
  const char* slug;
  const char* description;
  uint32_t coverageProbe;
};
inline constexpr std::array<ScriptFontSpec,14> scriptFontSpecs{{
  {"cjk","CJK (Japanese, shared Han)",0x3042},
  {"korean","Korean (Hangul)",0xAC00},
  {"devanagari","Devanagari (Hindi, Marathi, Nepali)",0x0915},
  {"bengali","Bengali (Bangla, Assamese)",0x0995},
  {"gujarati","Gujarati",0x0A95},
  {"gurmukhi","Gurmukhi (Punjabi)",0x0A15},
  {"kannada","Kannada",0x0C95},
  {"malayalam","Malayalam",0x0D15},
  {"oriya","Odia",0x0B15},
  {"sinhala","Sinhala",0x0D9A},
  {"tamil","Tamil",0x0B95},
  {"telugu","Telugu",0x0C15},
  {"arabic","Arabic",0x0627},
  {"hebrew","Hebrew",0x05D0},
}};

class ScriptFontFallback {
  std::vector<std::unique_ptr<SdCardFont>> loaded_;
  std::vector<int> ids_;
  uint32_t mask_=0;
 public:
  ScriptFontFallback()=default;
  ScriptFontFallback(const ScriptFontFallback&)=delete;
  ScriptFontFallback& operator=(const ScriptFontFallback&)=delete;
  uint32_t coverageMask() const {return mask_;}
  bool hasScript(size_t index) const {
    return index<scriptFontSpecs.size() && (mask_&(uint32_t(1)<<index));
  }
  void clear(GfxRenderer& renderer) {
    renderer.clearFallbackFonts();
    for(int id:ids_)renderer.removeFont(id);
    ids_.clear();
    loaded_.clear();
    mask_=0;
  }
  void load(GfxRenderer& renderer,int size,
            std::initializer_list<int> primaryIds) {
    clear(renderer);
    const char* disabled=std::getenv("CROSSPOD_DISABLE_CJK");
    for(size_t i=0;i<scriptFontSpecs.size();++i) {
      if(i==0 && disabled && std::strcmp(disabled,"1")==0)continue;
      // Test/debug override to isolate one script without uninstalling fonts.
      const char* excluded=std::getenv("CROSSPOD_DISABLE_SCRIPTS");
      if(excluded && *excluded) {
        const std::string list=std::string(",")+excluded+",";
        const std::string key=std::string(",")+scriptFontSpecs[i].slug+",";
        if(list.find(key)!=std::string::npos)continue;
      }
      const auto& spec=scriptFontSpecs[i];
      const std::string path=typographyRoot()+"/fonts/"+
        spec.slug+"-"+std::to_string(size)+".cpfont";
      auto font=std::make_unique<SdCardFont>();
      if(!font->load(path.c_str()) || !font->getEpdFont() ||
         !font->getEpdFont()->hasCodepoint(spec.coverageProbe))continue;
      const int id=400+static_cast<int>(i);
      renderer.insertFont(id,EpdFontFamily(size,font->getEpdFont()));
      renderer.registerSdCardFont(id,font.get());
      for(int primary:primaryIds)renderer.addFallbackFont(primary,id);
      loaded_.push_back(std::move(font));
      ids_.push_back(id);
      mask_|=uint32_t(1)<<i;
    }
    std::printf("CrossPod CJK fallback: %s %dpt\n",
      hasScript(0)?"original CrossPoint NotoSansCJKJP cpfont":"unavailable",
      size);
    std::printf("CrossPod multilingual fonts: %zu/%zu scripts loaded (%dpt)\n",
      loaded_.size(),scriptFontSpecs.size(),size);
  }
};
} // namespace crosspod
