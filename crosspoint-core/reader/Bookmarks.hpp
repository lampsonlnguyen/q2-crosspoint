// SPDX-License-Identifier: MIT
#pragma once
// Per-book bounded bookmarks. All writes are user-initiated and atomic to
// avoid wear from page turns. Stable content offsets allow repagination.
#include "ReadingPosition.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
namespace crosspod {
struct Bookmark {
  int spine=0;
  size_t page=0;
  uint32_t textOffset=0;
};
inline std::string bookmarksFile(const std::string& book) {
  return positionFile(book)+".bookmarks";
}
inline bool bookIdentity(const std::string& book,unsigned long long& size,
                         unsigned long long& modified) {
  struct stat st{};
  if(stat(book.c_str(),&st)!=0 || !S_ISREG(st.st_mode))return false;
  size=static_cast<unsigned long long>(st.st_size);
  modified=static_cast<unsigned long long>(st.st_mtime);
  return true;
}
inline std::vector<Bookmark> loadBookmarks(const std::string& book) {
  std::vector<Bookmark> result;
  unsigned long long size=0,mtime=0;
  if(!bookIdentity(book,size,mtime))return result;
  FILE* f=std::fopen(bookmarksFile(book).c_str(),"rb");
  if(!f)return result;
  char magic[32]{};
  unsigned long long storedSize=0,storedTime=0;
  unsigned count=0;
  const int head=std::fscanf(f,"%31s %llu %llu %u",magic,
                              &storedSize,&storedTime,&count);
  if(head!=4 || std::string(magic)!="CROSSPOD-BOOKMARKS1" ||
     storedSize!=size||storedTime!=mtime||count>32) {
    std::fclose(f);return result;
  }
  for(unsigned i=0;i<count;++i) {
    int spine=-1;
    unsigned long long page=0;
    unsigned offset=0;
    if(std::fscanf(f,"%d %llu %u",&spine,&page,&offset)!=3 ||
       spine<0||spine>100000||page>1000000ULL) {
      result.clear();break;
    }
    result.push_back({spine,static_cast<size_t>(page),offset});
  }
  std::fclose(f);
  return result;
}
inline bool saveBookmarks(const std::string& book,
                          const std::vector<Bookmark>& marks) {
  if(marks.size()>32)return false;
  unsigned long long size=0,mtime=0;
  if(!bookIdentity(book,size,mtime))return false;
  const auto path=bookmarksFile(book);
  const auto tmp=path+".tmp";
  FILE* f=std::fopen(tmp.c_str(),"wb");
  if(!f)return false;
  bool ok=std::fprintf(f,"CROSSPOD-BOOKMARKS1 %llu %llu %zu\n",
     size,mtime,marks.size())>0;
  for(const auto& mark:marks) {
    if(mark.spine<0||mark.spine>100000||mark.page>1000000) {ok=false;break;}
    if(std::fprintf(f,"%d %llu %u\n",mark.spine,
       static_cast<unsigned long long>(mark.page),mark.textOffset)<=0) {
      ok=false;break;
    }
  }
  if(std::fflush(f)!=0)ok=false;
  if(ok&&fsync(fileno(f))!=0)ok=false;
  if(std::fclose(f)!=0)ok=false;
  if(ok&&std::rename(tmp.c_str(),path.c_str())==0)return true;
  std::remove(tmp.c_str());return false;
}
inline bool addBookmark(const std::string& book,Bookmark mark) {
  auto marks=loadBookmarks(book);
  const bool duplicate=std::any_of(marks.begin(),marks.end(),
     [&](const Bookmark& m){return m.spine==mark.spine &&
       (m.textOffset==mark.textOffset || m.page==mark.page);});
  if(duplicate)return true;
  if(marks.size()>=32)return false;
  marks.push_back(mark);
  return saveBookmarks(book,marks);
}
} // namespace crosspod
