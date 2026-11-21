// SPDX-License-Identifier: MIT
#include "ReaderPreferences.hpp"
#include "Typography.hpp"
#include "Bookmarks.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
int main() {
  char root[]="/tmp/crosspod-state-tests-XXXXXX";
  char* dir=mkdtemp(root);
  assert(dir);
  assert(setenv("CROSSPOD_STATE_DIR",dir,1)==0);
  const std::string book=std::string(dir)+"/book.epub";
  FILE* f=std::fopen(book.c_str(),"wb");
  assert(f);
  assert(std::fwrite("book",1,4,f)==4);
  assert(std::fclose(f)==0);

  // UI font is independent; persisted oversized reader settings migrate to 12pt.
  crosspod::Typography textSize{};
  assert(crosspod::validTypography(textSize));
  crosspod::Typography oversizedSans{crosspod::Typography::Family::Sans,14};
  crosspod::Typography oversizedSerif{crosspod::Typography::Family::Serif,18};
  assert(!crosspod::validTypography(oversizedSans));
  assert(!crosspod::validTypography(oversizedSerif));
  {
    const std::string oldSetting=std::string(dir)+"/typography.conf";
    FILE* old=std::fopen(oldSetting.c_str(),"wb");
    assert(old);
    assert(std::fputs("CROSSPOD-TYPE1 sans 18\\n",old)>0);
    assert(std::fclose(old)==0);
    assert(crosspod::loadTypography(textSize));
    assert(textSize.size==12);
    std::remove(oldSetting.c_str());
  }
  crosspod::ReaderPreferences defaults;
  assert(!crosspod::loadReaderPreferences(defaults));
  assert(defaults.margin==14 && defaults.linePercent==100);
  crosspod::ReaderPreferences chosen{22,140,1,true,false};
  assert(crosspod::validReaderPreferences(chosen));
  assert(crosspod::saveReaderPreferences(chosen));
  assert(crosspod::loadReaderPreferences(defaults));
  assert(defaults.margin==22 && defaults.linePercent==140 &&
         defaults.alignment==1 && defaults.hyphenation && !defaults.embeddedStyle);
  auto invalid=chosen;invalid.margin=100;
  assert(!crosspod::saveReaderPreferences(invalid));

  assert(crosspod::loadBookmarks(book).empty());
  assert(crosspod::addBookmark(book,{0,3,100}));
  assert(crosspod::addBookmark(book,{0,3,100})); // de-dup
  assert(crosspod::addBookmark(book,{2,5,890}));
  auto marks=crosspod::loadBookmarks(book);
  assert(marks.size()==2);
  assert(marks[0].spine==0&&marks[0].page==3&&marks[0].textOffset==100);
  assert(marks[1].spine==2&&marks[1].page==5&&marks[1].textOffset==890);
  for(int i=3;i<=32;++i)
    assert(crosspod::addBookmark(book,{i,static_cast<size_t>(i),static_cast<uint32_t>(i*200)}));
  assert(crosspod::loadBookmarks(book).size()==32);
  assert(!crosspod::addBookmark(book,{33,33,3333})); // fixed limit

  // Modification changes identity and invalidates stale bookmarks.
  f=std::fopen(book.c_str(),"ab");
  assert(f);assert(std::fwrite("x",1,1,f)==1);std::fclose(f);
  assert(crosspod::loadBookmarks(book).empty());
  std::remove(crosspod::bookmarksFile(book).c_str());
  std::remove(crosspod::readerPreferencesFile().c_str());
  std::remove(book.c_str());
  assert(rmdir(dir)==0);
  return 0;
}
