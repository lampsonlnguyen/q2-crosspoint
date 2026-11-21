// SPDX-License-Identifier: MIT
#pragma once
// Q2-specific two-family typography preferences. Font files are CrossPoint
// CPFONT v4: their rasterized point size is part of the file, not a scale factor.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
namespace crosspod {
struct Typography {
  enum class Family { Sans, Serif };
  Family family=Family::Sans;
  int size=12;
};
inline bool validTypography(const Typography& t) {
  return (t.family==Typography::Family::Sans ||
          t.family==Typography::Family::Serif) &&
         (t.size==12 || t.size==14 || t.size==16 || t.size==18);
}
inline const char* typographyFamilyName(Typography::Family f) {
  return f==Typography::Family::Serif?"Serif":"Sans";
}
inline std::string typographyRoot() {
  const char* env=std::getenv("CROSSPOD_STATE_DIR");
  return env && *env ? env : "/mnt/mmc/.crosspod";
}
inline std::string typographyFontPath(Typography t) {
  if(!validTypography(t))return {};
  return typographyRoot()+"/fonts/"+
    (t.family==Typography::Family::Serif?std::string("serif-"):std::string("sans-"))+
    std::to_string(t.size)+".cpfont";
}
// Shared CJK fallback is the original CrossPoint NotoSansCJKJP .cpfont v4.
// This does not replace the reader's selected Sans/Serif face for Latin text.
inline std::string typographyCjkFontPath(int size) {
  if(size!=12 && size!=14 && size!=16 && size!=18)return {};
  return typographyRoot()+"/fonts/cjk-"+std::to_string(size)+".cpfont";
}
inline bool loadTypography(Typography& value) {
  const std::string path=typographyRoot()+"/typography.conf";
  std::FILE* f=std::fopen(path.c_str(),"rb");
  if(!f)return false;
  char family[16]={};
  int size=0;
  const int count=std::fscanf(f,"CROSSPOD-TYPE1 %15s %d",family,&size);
  std::fclose(f);
  if(count!=2)return false;
  Typography read{std::strcmp(family,"serif")==0?
    Typography::Family::Serif:Typography::Family::Sans,size};
  if(std::strcmp(family,"serif")!=0 && std::strcmp(family,"sans")!=0)return false;
  if(!validTypography(read))return false;
  value=read;
  return true;
}
inline bool saveTypography(Typography t) {
  if(!validTypography(t))return false;
  const std::string filename=typographyRoot()+"/typography.conf";
  const std::string temp=filename+".tmp";
  std::FILE* f=std::fopen(temp.c_str(),"wb");
  if(!f)return false;
  const bool wrote=std::fprintf(f,"CROSSPOD-TYPE1 %s %d\n",
      t.family==Typography::Family::Serif?"serif":"sans",t.size)>0;
  bool ok=wrote;
  if(std::fflush(f)!=0)ok=false;
  if(ok && fsync(fileno(f))!=0)ok=false;
  if(std::fclose(f)!=0)ok=false;
  if(ok && std::rename(temp.c_str(),filename.c_str())==0)return true;
  std::remove(temp.c_str());
  return false;
}
} // namespace crosspod
