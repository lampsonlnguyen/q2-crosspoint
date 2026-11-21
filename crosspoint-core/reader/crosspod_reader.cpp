// SPDX-License-Identifier: MIT
// First real EPUB-to-Q2 vertical slice: original CrossPoint ZIP/OPF, ParsedText,
// TextBlock, GfxRenderer and font rasterizer, with POSIX XML and Q2 display/input.
// XHTML structure extraction is provisional. CSS, images, ruby and links are
// NOT faithfully rendered until original ChapterHtmlSlimParser is ported.
#include <BidiUtils.h>
#include <BookMetadataCache.h>
#include <ContainerParser.h>
#include <ContentOpfParser.h>
#include <CssParser.h>
#include <HalStorage.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <ParsedText.h>
#include <Page.h>
#include <Section.h>
#include <ReaderRenderSpec.h>
#include <Epub/blocks/ImageBlock.h>
#include <Q2Controls.hpp>
#include <SdCardFont.h>
#include "Typography.hpp"
#include "ScriptFonts.hpp"
#include "ReadingPosition.hpp"
#include "ReaderPreferences.hpp"
#include "Bookmarks.hpp"
#include "PngPreview.hpp"
#include "JpegPreview.hpp"
#include <Q2SocketInput.hpp>
#include <ZipFile.h>
#include <builtinFonts/notosans_12_regular.h>
#include <expat.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <sys/resource.h>
#include <dirent.h>
#include <cctype>
#include <unistd.h>

namespace {
constexpr size_t MAX_XML_BYTES = 4 * 1024 * 1024;
constexpr size_t MAX_TEXT_BYTES = 2 * 1024 * 1024;
constexpr size_t MAX_BLOCKS = 16000;
constexpr size_t MAX_PAGES_PER_SPINE = 512;
constexpr int SCREEN_H = 320, LEFT = 14, TOP = 16;
constexpr int TEXT_WIDTH = 345, BOTTOM = 306;
volatile std::sig_atomic_t interrupted = 0;
std::string clippedLabel(const std::string&,size_t);
void exitOnSignal(int) { interrupted = 1; }

std::string_view localName(std::string_view tag) {
  const size_t separator = tag.find_last_of("|:");
  if (separator != std::string_view::npos) tag.remove_prefix(separator + 1);
  return tag;
}
bool isParagraph(std::string_view n) {
  return n == "p" || n == "h1" || n == "h2" || n == "h3" ||
         n == "h4" || n == "h5" || n == "h6" || n == "li" ||
         n == "blockquote" || n == "pre" || n == "dt" || n == "dd" ||
         n == "div" || n == "section" || n == "article" ||
         n == "figcaption" || n == "caption" || n == "td" || n == "th";
}
bool skipTag(std::string_view n) {
  return n == "script" || n == "style" || n == "svg" || n == "nav";
}
// Resolve a relative EPUB ZIP resource without allowing traversal outside
// the archive. Relative ../ segments are common for CSS and XHTML images.
std::string resolveEpubResource(const std::string& chapterDir,std::string_view uri) {
  if(uri.empty() || uri.front()=='/' || uri.front()=='\\' ||
     uri.find(':')!=std::string_view::npos ||
     uri.find(static_cast<char>(92))!=std::string_view::npos ||
     uri.find('?')!=std::string_view::npos ||
     uri.find('#')!=std::string_view::npos)return {};
  std::vector<std::string> segments;
  const std::string joined=chapterDir+std::string(uri);
  size_t cursor=0;
  while(cursor<joined.size()) {
    const size_t next=joined.find('/',cursor);
    const auto segment=joined.substr(cursor,next==std::string::npos?
                                            std::string::npos:next-cursor);
    if(segment=="..") {
      if(segments.empty())return {};
      segments.pop_back();
    } else if(!segment.empty() && segment!=".")segments.push_back(segment);
    if(next==std::string::npos)break;
    cursor=next+1;
  }
  if(segments.empty())return {};
  std::string normalized;
  for(const auto& segment:segments) {
    if(!normalized.empty())normalized.push_back('/');
    normalized+=segment;
  }
  return normalized;
}
struct StyledRun {
  std::string text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
};
struct Paragraph {
  std::vector<StyledRun> runs;
  CssTextAlign align = CssTextAlign::Justify;
  bool rtl = false;
  bool heading = false;
  BlockStyle block; // original CrossPoint CSS box style
  std::string imagePath; // EPUB ZIP path, bounded PNG or JPEG resource.
};
struct HtmlCollector {
  // The full upstream ChapterHtmlSlimParser is being integrated separately.
  // This transitional adapter uses the ORIGINAL CrossPoint CSS cascade,
  // rather than interpreting CSS with a Q2-specific parser.
  ZipFile& archive;
  std::string chapterDir;
  CssParser stylesheet{""};
  std::vector<Paragraph> items;
  std::array<CssAncestor, 16> ancestors{};
  struct Scope {
    bool hidden=false, bold=false, italic=false, rtl=false;
    CssTextAlign align=CssTextAlign::Justify;
    bool paragraph=false;
    BlockStyle paragraphBlock{};
    bool heading=false;
    bool stylesheet=false;
    bool pageBreakAfter=false;
    int listKind=0; // 0 none, 1 unordered, 2 ordered
    unsigned nextItemNumber=1;
  };
  std::vector<Scope> scopes;
  Paragraph current;
  std::string cssText;
  int collectingDepth=0;
  size_t collectedBytes=0;
  bool limitHit=false;

  HtmlCollector(ZipFile& zip, const std::string& chapter) : archive(zip) {
    const size_t slash=chapter.find_last_of('/');
    if(slash!=std::string::npos) chapterDir=chapter.substr(0,slash+1);
    scopes.push_back({});
  }
  static std::string_view attribute(const XML_Char** attrs, std::string_view key) {
    if(!attrs)return {};
    for(size_t i=0;attrs[i];i+=2)
      if(attrs[i]==key)return attrs[i+1];
    return {};
  }
  bool addCss(std::string_view content) {
    if(content.empty() || content.size()>256*1024)return false;
    HalFile file(std::tmpfile());
    if(!file || file.write(content.data(),content.size())!=content.size() || !file.seek(0))
      return false;
    return stylesheet.loadFromStream(file)!=CssParser::ParseResult::Error;
  }
  void externalCss(std::string_view uri) {
    const std::string path=resolveEpubResource(chapterDir,uri);
    if(path.empty())return;
    size_t len=0;
    if(!archive.getInflatedFileSize(path.c_str(),&len)||len==0||len>256*1024)return;
    std::unique_ptr<uint8_t,decltype(&std::free)> bytes(
      archive.readFileToMemory(path.c_str(),&len,true),&std::free);
    if(bytes)(void)addCss(std::string_view(reinterpret_cast<const char*>(bytes.get()),len));
  }
  static void XMLCALL start(void* user, const XML_Char* name, const XML_Char** attrs) {
    auto& s=*static_cast<HtmlCollector*>(user);
    const auto n=localName(name);
    const auto& parent=s.scopes.back();
    Scope scope=parent;
    scope.paragraph=false;
    scope.stylesheet=false;
    scope.pageBreakAfter=false;
    if(n=="ol" || n=="ul") {
      scope.listKind=n=="ol"?2:1;
      scope.nextItemNumber=1;
      // Honor EPUB ordered-list start values with bounded numeric parsing.
      if(n=="ol") {
        const auto startValue=attribute(attrs,"start");
        if(!startValue.empty() && startValue.size()<=5) {
          unsigned number=0;
          bool valid=true;
          for(const unsigned char digit:startValue) {
            if(digit<'0' || digit>'9') {valid=false;break;}
            number=number*10+digit-'0';
          }
          if(valid && number>0 && number<=99999)scope.nextItemNumber=number;
        }
      }
    }
    const std::string_view cls=attribute(attrs,"class");
    const std::string_view id=attribute(attrs,"id");
    if(n=="link") {
      const auto rel=attribute(attrs,"rel");
      if(rel=="stylesheet")s.externalCss(attribute(attrs,"href"));
    }
    CssStyle style;
    if(!scope.hidden) {
      const size_t depth=s.scopes.size()-1;
      style=s.stylesheet.resolveStyle(n,cls,id,s.ancestors.data(),
          std::min(depth,s.ancestors.size()));
      const auto inlineCss=attribute(attrs,"style");
      if(!inlineCss.empty())style.applyOver(CssParser::parseInlineStyle(inlineCss));
      if(depth<s.ancestors.size())
        s.ancestors[depth]=CssParser::makeAncestor(n,cls,id);
    }
    scope.hidden=scope.hidden || skipTag(n) || n=="head" || n=="title" ||
      n=="rp" || !attribute(attrs,"hidden").empty() ||
      (style.hasDisplay() && style.display==CssDisplay::None);
    // A stylesheet is captured in head even though head is not visible content.
    scope.stylesheet=(n=="style");
    if(n=="b" || n=="strong")scope.bold=true;
    if(n=="i" || n=="em")scope.italic=true;
    if(style.hasFontWeight())scope.bold=style.fontWeight==CssFontWeight::Bold;
    if(style.hasFontStyle())scope.italic=style.fontStyle==CssFontStyle::Italic;
    if(style.hasTextAlign())scope.align=style.textAlign;
    if(style.hasDirection())scope.rtl=style.direction==CssTextDirection::Rtl;
    const auto dir=attribute(attrs,"dir");
    if(dir=="rtl")scope.rtl=true;
    else if(dir=="ltr")scope.rtl=false;
    if(!scope.hidden && isParagraph(n)) {
      // A new block inside another block must not disappear. Commit the
      // visible parent text before entering a nested paragraph/list/table
      // cell, then restore the parent after the child closes.
      if(s.collectingDepth && !s.current.runs.empty() &&
         s.items.size()<MAX_BLOCKS) {
        s.items.push_back(std::move(s.current));
      }
      scope.paragraph=true;
      s.collectingDepth=static_cast<int>(s.scopes.size());
      s.current=Paragraph{};
      s.current.align=scope.align;
      s.current.rtl=scope.rtl;
      s.current.heading=n.size()==2 && n[0]=='h';
      // Use original CrossPoint block length resolution, not a Q2 CSS parser.
      s.current.block=BlockStyle::fromCssStyle(
          style,27.0f,CssTextAlign::None,TEXT_WIDTH);
      s.current.block.alignment=scope.align;
      s.current.block.textAlignDefined=true;
      s.current.block.isRtl=scope.rtl;
      s.current.block.directionDefined=style.hasDirection() || !dir.empty();
      scope.pageBreakAfter=s.current.block.pageBreakAfter;
      scope.paragraphBlock=s.current.block;
      scope.heading=s.current.heading;
      if(n=="li") {
        // Preserve list structure instead of silently concatenating item text.
        std::string markerText="- ";
        if(scope.listKind==2) {
          const unsigned number=s.scopes.back().nextItemNumber++;
          markerText=std::to_string(number)+". ";
        }
        s.current.runs.push_back({std::move(markerText),EpdFontFamily::REGULAR});
      }
    }
    s.scopes.push_back(scope);
    if(n=="br" && s.collectingDepth && !scope.hidden) {
      // A hard XHTML line break is not a space: commit the current visual
      // line and continue with a new block, retaining inherited styling.
      BlockStyle retained=s.current.block;
      if(!s.current.runs.empty() && s.items.size()<MAX_BLOCKS)
        s.items.push_back(std::move(s.current));
      s.current=Paragraph{};
      s.current.align=scope.align;
      s.current.rtl=scope.rtl;
      retained.pageBreakBefore=false;
      retained.pageBreakAfter=false;
      retained.marginTop=retained.paddingTop=0;
      s.current.block=retained;
    }
    if((n=="img" || n=="image") && !scope.hidden) {
      const auto uri=attribute(attrs,n=="img"?"src":"href");
      // An EPUB image may live in a sibling directory (../images).
      const std::string resolved=resolveEpubResource(s.chapterDir,uri);
      if(!resolved.empty() && s.items.size()<MAX_BLOCKS) {
        const auto ext=resolved.find_last_of('.');
        std::string extension=ext==std::string::npos?"" :resolved.substr(ext);
        std::transform(extension.begin(),extension.end(),extension.begin(),
                       [](unsigned char c){return static_cast<char>(std::tolower(c));});
        if(extension==".png" || extension==".jpg" || extension==".jpeg") {
          if(s.collectingDepth && !s.current.runs.empty()) {
            s.items.push_back(std::move(s.current));
            s.current=Paragraph{};
            s.current.align=scope.align;
            s.current.rtl=scope.rtl;
          }
          Paragraph image;
          image.imagePath=resolved;
          s.items.push_back(std::move(image));
        }
      }
    }
  }
  static void XMLCALL data(void* user, const XML_Char* bytes, int len) {
    auto& s=*static_cast<HtmlCollector*>(user);
    if(len<=0 || s.limitHit)return;
    const Scope& scope=s.scopes.back();
    if(scope.stylesheet && s.cssText.size()+static_cast<size_t>(len)<=256*1024)
      s.cssText.append(bytes,static_cast<size_t>(len));
    if(!s.collectingDepth || scope.hidden)return;
    if(s.collectedBytes+static_cast<size_t>(len)>MAX_TEXT_BYTES) {
      s.limitHit=true;return;
    }
    auto style=EpdFontFamily::REGULAR;
    if(scope.bold)style=static_cast<EpdFontFamily::Style>(style|EpdFontFamily::BOLD);
    if(scope.italic)style=static_cast<EpdFontFamily::Style>(style|EpdFontFamily::ITALIC);
    if(!s.current.runs.empty() && s.current.runs.back().style==style)
      s.current.runs.back().text.append(bytes,static_cast<size_t>(len));
    else s.current.runs.push_back({std::string(bytes,static_cast<size_t>(len)),style});
    s.collectedBytes+=static_cast<size_t>(len);
  }
  static void XMLCALL end(void* user, const XML_Char* name) {
    auto& s=*static_cast<HtmlCollector*>(user);
    if(s.scopes.size()<=1)return;
    const auto scope=s.scopes.back();
    if(localName(name)=="style" && !s.cssText.empty()) {
      (void)s.addCss(s.cssText);
      s.cssText.clear();
    }
    if(scope.paragraph && static_cast<int>(s.scopes.size())-1==s.collectingDepth) {
      bool nonempty=false;
      for(const auto& run:s.current.runs)
        if(run.text.find_first_not_of(" \t\r\n")!=std::string::npos)nonempty=true;
      if(nonempty && s.items.size()<MAX_BLOCKS)
        s.items.push_back(std::move(s.current));
      else if(s.items.size()>=MAX_BLOCKS)s.limitHit=true;
      if(scope.pageBreakAfter && !s.items.empty())
        s.items.back().block.pageBreakAfter=true;
      s.collectingDepth=0;
      // Resume text after a nested <p>, <li>, <td> or <div> rather than
      // discarding the parent's trailing words.
      for(int d=static_cast<int>(s.scopes.size())-2;d>=1;--d) {
        if(!s.scopes[d].paragraph)continue;
        const auto& parent=s.scopes[d];
        s.collectingDepth=d;
        s.current=Paragraph{};
        s.current.align=parent.align;
        s.current.rtl=parent.rtl;
        s.current.heading=parent.heading;
        s.current.block=parent.paragraphBlock;
        s.current.block.pageBreakBefore=false;
        s.current.block.pageBreakAfter=false;
        s.current.block.marginTop=s.current.block.paddingTop=0;
        break;
      }
    }
    s.scopes.pop_back();
  }
};
// Stream ZIP inflation into Expat. A chapter can be several megabytes; holding
// its entire uncompressed XML alongside the CSS, pagination and image buffers
// needlessly spikes peak RAM on the Q2. This adapter uses CrossPoint's ZipFile
// streaming contract and caps both input and collected text.
class ChapterXmlSink final : public Print {
  XML_Parser parser_;
  size_t seen_=0;
  bool failed_=false;
 public:
  explicit ChapterXmlSink(XML_Parser parser):parser_(parser) {}
  size_t write(uint8_t byte) override {return write(&byte,1);}
  size_t write(const uint8_t* data,size_t len) override {
    if(failed_ || !data || len>MAX_XML_BYTES-seen_ ||
       len>static_cast<size_t>(INT_MAX)) {
      failed_=true;
      return 0;
    }
    if(!len)return 0;
    seen_+=len;
    if(XML_Parse(parser_,reinterpret_cast<const char*>(data),
                 static_cast<int>(len),XML_FALSE)!=XML_STATUS_OK) {
      failed_=true;
      return 0;
    }
    return len;
  }
  bool finish() {
    return !failed_ && seen_!=0 &&
        XML_Parse(parser_,nullptr,0,XML_TRUE)==XML_STATUS_OK;
  }
};

bool parseChapter(ZipFile& archive,const std::string& bookPath,
                  const std::string& name,std::vector<Paragraph>& out) {
  size_t size=0;
  if(!archive.getInflatedFileSize(name.c_str(),&size)||!size||size>MAX_XML_BYTES) {
    std::fprintf(stderr,"CrossPod: unsupported XHTML chapter length: %s (%zu)\n",name.c_str(),size);
    return false;
  }
  XML_Parser parser=XML_ParserCreateNS(nullptr,'|');
  if(!parser)return false;
  HtmlCollector collector(archive,name);
  XML_SetUserData(parser,&collector);
  XML_SetElementHandler(parser,HtmlCollector::start,HtmlCollector::end);
  XML_SetCharacterDataHandler(parser,HtmlCollector::data);
  ChapterXmlSink sink(parser);
  // CSS <link> callbacks also read from the archive. Do not seek the ZIP
  // currently being inflated: use a separate ZipFile cursor for streaming.
  ZipFile chapterStream(bookPath);
  const bool inflated=chapterStream.readFileToStream(name.c_str(),sink,1024);
  const bool ok=inflated && sink.finish();
  if(!ok)std::fprintf(stderr,"CrossPod: XHTML stream/parse error %s: %s\n",
      name.c_str(),XML_ErrorString(XML_GetErrorCode(parser)));
  XML_ParserFree(parser);
  if(!ok||collector.limitHit||collector.items.empty())return false;
  out=std::move(collector.items);
  return true;
}
class TempCache {
  std::string dir_;
 public:
  TempCache() {
    char dir[]="/tmp/crosspod-epub-XXXXXX";
    if (char* d=mkdtemp(dir)) dir_=d;
  }
  const std::string& path() const { return dir_; }
  ~TempCache() {
    if (dir_.empty()) return;
    // CrossPoint cache owns temporary .items/.spine/.toc and book.bin files.
    // The private directory is only used by this process.
    for (const char* name: {"book.bin","spine.bin.tmp","toc.bin.tmp",
                            ".items.bin"}) {
      std::remove((dir_+"/"+name).c_str());
    }
    // Removal of any remaining entries is intentionally NOT recursive.
    if (rmdir(dir_.c_str()) != 0)
      std::fprintf(stderr,"CrossPod: temporary cache at %s requires cleanup\n",dir_.c_str());
  }
};

struct RenderLine {
  std::unique_ptr<TextBlock> block;
  int y = 0;
};
struct RenderImage {
  crosspod::PngPreview png;
  int x=0,y=0,width=0,height=0;
};
struct RenderPage {
  std::vector<RenderLine> lines;
  std::vector<RenderImage> images;
};
class Reader {
  std::string bookPath_;
  ZipFile archive_;
  TempCache temporary_;
  BookMetadataCache metadata_;
  HalDisplay display_;
  GfxRenderer renderer_;
  EpdFont bodyFont_;
  FontDecompressor decompressor_;
  FontCacheManager fontCache_;
  std::unique_ptr<SdCardFont> selectedFont_;
  crosspod::ScriptFontFallback scriptFonts_;
  // Keep the complete upstream EPUB parser/Section lifecycle alive for this
  // book. CrossPod's prior collector remains a fail-safe fallback.
  std::shared_ptr<Epub> upstreamEpub_;
  std::unique_ptr<Section> upstreamSection_;
  int upstreamFontId_=0;
  crosspod::Typography typography_{};
  crosspod::ReaderPreferences preferences_{};
  std::string typographyNotice_;
  std::vector<RenderPage> pages_;
  // A bounded 3-page 1-bpp raster cache (~45 KB) avoids repeated SD-page
  // decompression, layout painting and glyph lookups when moving back/forward.
  struct CachedRaster {
    int spine=-1;
    size_t page=0;
    std::vector<uint8_t> pixels;
  };
  std::array<CachedRaster,3> rasterCache_{};
  size_t nextRasterSlot_=0;
  void clearRasterCache() {
    for(auto& item:rasterCache_)item.spine=-1;
  }
  int currentSpine_ = -1;
  size_t currentPage_ = 0;
  bool resumeEnabled_ = false;
  mutable bool positionDirty_=false;
  mutable std::chrono::steady_clock::time_point lastPositionSync_{};
  int lineHeight() const { return std::max(22,typography_.size*2); }
  size_t pageCount() const {
    return upstreamSection_?upstreamSection_->pageCount:pages_.size();
  }
  int fontIdForTypography(crosspod::Typography value) const {
    return 100+(value.family==crosspod::Typography::Family::Serif?4:0)+
           (value.size-12)/2;
  }
  ReaderRenderSpec upstreamSpec() const {
    ReaderRenderSpec spec;
    spec.fontId=upstreamFontId_;
    spec.viewportWidth=static_cast<uint16_t>(375-2*preferences_.margin-2);
    spec.viewportHeight=BOTTOM-TOP;
    spec.lineCompression=preferences_.linePercent/100.0f;
    spec.paragraphAlignment=static_cast<uint8_t>(preferences_.alignment);
    spec.hyphenationEnabled=preferences_.hyphenation;
    spec.embeddedStyle=preferences_.embeddedStyle;
    spec.lineCompression=1.0f;
    spec.embeddedStyle=true;
    spec.imageRendering=0;
    spec.hyphenationEnabled=false;
    return spec;
  }
  std::string upstreamCachePath() const {
    struct stat st{};
    (void)::stat(bookPath_.c_str(),&st);
    uint64_t hash=14695981039346656037ull;
    const auto mix=[&](uint64_t value) {
      for(int i=0;i<8;++i){hash^=(value>>(8*i))&255u;hash*=1099511628211ull;}
    };
    for(unsigned char byte:bookPath_){hash^=byte;hash*=1099511628211ull;}
    mix(static_cast<uint64_t>(st.st_size));
    mix(static_cast<uint64_t>(st.st_mtime));
    // CJK font coverage changes line measurement and pagination. Never reuse
    // Section layout cache from a Latin-only build after enabling CJK.
    mix(0x434a4b01ull);
    mix(static_cast<uint64_t>(scriptFonts_.coverageMask()));
    const char* state=std::getenv("CROSSPOD_STATE_DIR");
    struct stat cardDir{};
    const bool cardMounted=::stat("/mnt/mmc/.crosspod",&cardDir)==0 &&
                          S_ISDIR(cardDir.st_mode);
    const std::string base=state&&*state?state:
                           (cardMounted?"/mnt/mmc/.crosspod":"/tmp");
    const std::string cache=base+"/cache";
    Storage.mkdir(cache.c_str());
    char folder[44];
    std::snprintf(folder,sizeof(folder),"book-%016llx",
                  static_cast<unsigned long long>(hash));
    return cache+"/"+folder;
  }
  static bool extractUpstreamImage(void* context,const char* source,const char* dest) {
    auto* self=static_cast<Reader*>(context);
    if(!self || !self->upstreamEpub_ || !source || !dest)return false;
    HalFile out;
    if(!Storage.openFileForWrite("CROSSPOD",dest,out))return false;
    const bool ok=self->upstreamEpub_->readItemContentsToStream(source,out,4096);
    out.close();
    if(!ok)Storage.remove(dest);
    return ok;
  }
  bool useTypography(crosspod::Typography settings) {
    if(!crosspod::validTypography(settings))return false;
    auto next=std::make_unique<SdCardFont>();
    if(!next->load(crosspod::typographyFontPath(settings).c_str()) ||
       !next->getEpdFont()) {
      if(settings.family!=crosspod::Typography::Family::Sans || settings.size!=12)
        return false;
      next.reset();
    }
    // Drop SD glyph registrations BEFORE their owning font handles. Rebuild
    // this size's script-coverage index without loading font bitmaps into RAM.
    scriptFonts_.clear(renderer_);
    renderer_.removeFont(1);
    if(upstreamFontId_)renderer_.removeFont(upstreamFontId_);
    const int nextFontId=fontIdForTypography(settings);
    if(next && next->getEpdFont()) {
      renderer_.insertFont(1,EpdFontFamily(settings.size,next->getEpdFont()));
      renderer_.registerSdCardFont(1,next.get());
      renderer_.insertFont(nextFontId,EpdFontFamily(settings.size,next->getEpdFont()));
      renderer_.registerSdCardFont(nextFontId,next.get());
    } else {
      renderer_.insertFont(1,EpdFontFamily(12,&bodyFont_));
      renderer_.insertFont(nextFontId,EpdFontFamily(12,&bodyFont_));
    }
    selectedFont_=std::move(next);
    upstreamFontId_=nextFontId;
    scriptFonts_.load(renderer_,settings.size,{1,nextFontId});
    typography_=settings;
    clearRasterCache();
    return true;
  }

  bool beginNextPage(int& y) {
    if (pages_.size() >= MAX_PAGES_PER_SPINE) return false;
    pages_.emplace_back();
    y=TOP;
    return true;
  }
  bool paginate(const std::vector<Paragraph>& paragraphs) {
    pages_.clear();
    pages_.emplace_back();
    int y=TOP;
    size_t totalLines=0;
    size_t totalImages=0;
    bool pageLimitReached=false;
    for (const auto& p:paragraphs) {
      if(p.block.pageBreakBefore &&
         (!pages_.back().lines.empty() || !pages_.back().images.empty()) &&
         !beginNextPage(y))return false;
      y+=std::clamp<int>(p.block.topInset(),0,48);
      if(y>=BOTTOM && (!pages_.back().lines.empty() ||
         !pages_.back().images.empty()) && !beginNextPage(y))return false;
      if(!p.imagePath.empty()) {
        size_t bytes=0;
        if(!archive_.getInflatedFileSize(p.imagePath.c_str(),&bytes) ||
           bytes==0 || bytes>4u*1024u*1024u)continue;
        std::unique_ptr<uint8_t,decltype(&std::free)> resource(
          archive_.readFileToMemory(p.imagePath.c_str(),&bytes,false),&std::free);
        crosspod::PngPreview png;
        if(!resource)continue;
        const size_t dot=p.imagePath.find_last_of('.');
        std::string extension=dot==std::string::npos?"" :p.imagePath.substr(dot);
        std::transform(extension.begin(),extension.end(),extension.begin(),
                       [](unsigned char c){return static_cast<char>(std::tolower(c));});
        const bool loaded=extension==".png"?
            crosspod::decodePngPreview(resource.get(),bytes,png):
            crosspod::decodeJpegPreview(resource.get(),bytes,png);
        if(!loaded)continue;
        const int width=std::min<int>(TEXT_WIDTH,png.width);
        const int height=std::max<int>(1,static_cast<int>(
          static_cast<int64_t>(png.height)*width/png.width));
        const int fitH=std::min(height,BOTTOM-TOP);
        const int fitW=std::max<int>(1,static_cast<int>(
          static_cast<int64_t>(width)*fitH/height));
        if(y+fitH>BOTTOM && !pages_.back().lines.empty() &&
           !beginNextPage(y))return false;
        if(y+fitH>BOTTOM && !pages_.back().images.empty() &&
           !beginNextPage(y))return false;
        pages_.back().images.push_back({
          std::move(png),LEFT+(TEXT_WIDTH-fitW)/2,y,fitW,fitH});
        ++totalImages;
        y+=fitH+8;
        continue;
      }
      BlockStyle block=p.block;
      block.alignment=p.align;
      block.textAlignDefined=true;
      block.isRtl=p.rtl;
      ParsedText layout(false,false,block,0);
      std::string pending;
      EpdFontFamily::Style pendingStyle=EpdFontFamily::REGULAR;
      // One word may cross inline span boundaries: preserve style runs without
      // converting any UTF-8 bytes or collapsing the whole chapter to ASCII.
      const auto flush=[&]() {
        if(!pending.empty())layout.addWord(std::move(pending),pendingStyle);
        pending.clear();
      };
      for(const auto& run:p.runs) {
        for(const unsigned char ch:run.text) {
          if(ch==' '||ch=='\t'||ch=='\r'||ch=='\n')flush();
          else {
            if(!pending.empty()&&run.style!=pendingStyle)flush();
            pendingStyle=run.style;
            pending.push_back(static_cast<char>(ch));
          }
        }
      }
      flush();
      if (layout.hadDroppedWords()) return false;
      const int inset=std::clamp<int>(block.totalHorizontalInset(),0,96);
      layout.layoutAndExtractLines(renderer_,1,
          static_cast<uint16_t>(TEXT_WIDTH-inset),
          [&](std::unique_ptr<TextBlock> line,uint32_t) {
            if (!line || !line->valid() || pages_.size() > MAX_PAGES_PER_SPINE) return;
            if (y + lineHeight() > BOTTOM && !beginNextPage(y)) {
              pageLimitReached=true;
              return;
            }
            pages_.back().lines.push_back({std::move(line),y});
            y+=lineHeight();
            ++totalLines;
          });
      if (pageLimitReached) {
        std::fputs("CrossPod: chapter exceeds the bounded page cache\n",stderr);
        return false;
      }
      y+=std::clamp<int>(p.block.bottomInset(),0,48);
      y+=p.heading?12:8;
      if(p.block.pageBreakAfter &&
         (!pages_.back().lines.empty() || !pages_.back().images.empty()) &&
         !beginNextPage(y))return false;
    }
    if (pages_.size()>1 && pages_.back().lines.empty() &&
        pages_.back().images.empty()) pages_.pop_back();
    if (pages_.empty() || (!totalLines && !totalImages)) return false;
    return true;
  }
 public:
  explicit Reader(std::string path):
      bookPath_(std::move(path)),archive_(bookPath_),
      metadata_(temporary_.path()),renderer_(display_),
      bodyFont_(&notosans_12_regular),
      fontCache_(renderer_.getFontMap(),renderer_.getSdCardFonts(),renderer_.getTtfFonts()) {}
  ~Reader() {
    flushPosition();
    ImageBlock::setExtractor(nullptr,nullptr);
  }
  bool open() {
    (void)crosspod::loadReaderPreferences(preferences_);
    if (temporary_.path().empty()) {
      std::fputs("CrossPod: cannot create temporary cache\n",stderr);return false;
    }
    if (!archive_.loadAllFileStatSlims()) {
      std::fputs("CrossPod: cannot index EPUB ZIP\n",stderr);return false;
    }
    size_t size=0;
    if (!archive_.getInflatedFileSize("META-INF/container.xml",&size) ||
        size>MAX_XML_BYTES || !size) return false;
    ContainerParser container(size);
    if (!container.setup() ||
        !archive_.readFileToStream("META-INF/container.xml",container,512) ||
        container.fullPath.empty()) return false;
    if (container.fullPath.front()=='/' ||
        container.fullPath.find("..")!=std::string::npos) return false;
    const std::string opfPath=container.fullPath;
    if (!archive_.getInflatedFileSize(opfPath.c_str(),&size) ||
        size>MAX_XML_BYTES || !size) return false;
    std::string opfDir;
    const auto slash=opfPath.find_last_of('/');
    if(slash!=std::string::npos) opfDir=opfPath.substr(0,slash+1);
    if(!metadata_.beginWrite() || !metadata_.beginContentOpfPass())return false;
    BookMetadataCache::BookMetadata info;
    {
      ContentOpfParser opf(temporary_.path(),opfDir,size,&metadata_,false);
      if(!opf.setup() || !archive_.readFileToStream(opfPath.c_str(),opf,1024))return false;
      info.title=opf.title;
      info.author=opf.author;
      info.language=opf.language;
      info.coverItemHref=opf.coverItemHref;
      info.textReferenceHref=opf.textReferenceHref;
    }
    if(!metadata_.endContentOpfPass() ||
       !metadata_.beginTocPass() || !metadata_.endTocPass() ||
       !metadata_.endWrite() || !metadata_.buildBookBin(bookPath_,info) ||
       !metadata_.load() || !metadata_.getSpineCount())return false;
    // These are unmodified upstream CrossPoint layout/render sources.
    display_.begin();
    renderer_.setOrientation(GfxRenderer::LandscapeCounterClockwise);
    renderer_.begin();
    renderer_.insertFont(1,EpdFontFamily(12,&bodyFont_));
    // UI font is always fixed; book typography updates must not touch it.
    renderer_.insertFont(2,EpdFontFamily(12,&bodyFont_));
    if (!decompressor_.init()) return false;
    fontCache_.setFontDecompressor(&decompressor_);
    renderer_.setFontCacheManager(&fontCache_);
    (void)useTypography(typography_); // extended Sans 12 when installed
    crosspod::Typography savedType;
    if(crosspod::loadTypography(savedType) && !useTypography(savedType))
      typographyNotice_="Selected font file missing";
    std::printf("CrossPod typeface: %s %dpt%s\n",
        crosspod::typographyFamilyName(typography_.family),typography_.size,
        typographyNotice_.empty()?"":" (saved font unavailable; fallback)");
    std::printf("CrossPod font source: %s\n",
        selectedFont_?"original CrossPoint extended cpfont":"built-in emergency font");
    std::printf("CrossPod EPUB: %s — %s | %d spine entries\n",
        info.title.c_str(),info.author.c_str(),metadata_.getSpineCount());
    const char* requestedEngine=std::getenv("CROSSPOD_ENGINE");
    if(!requestedEngine || std::strcmp(requestedEngine,"legacy")!=0) {
      auto candidate=std::make_shared<Epub>(bookPath_,upstreamCachePath());
      if(candidate->load(true,false)) {
        upstreamEpub_=std::move(candidate);
        ImageBlock::setExtractor(this,&Reader::extractUpstreamImage);
      } else std::fprintf(stderr,
          "CrossPod: original Section unavailable; using legacy collector\n");
    }
    return loadSpine(0,1);
  }
  void enableResume() {
    resumeEnabled_=true;
    crosspod::ReadingPosition saved;
    if(!crosspod::loadReadingPosition(bookPath_,saved))return;
    if(!loadSpine(saved.spine,1) || currentSpine_!=saved.spine) {
      (void)loadSpine(0,1);
      return;
    }
    currentPage_=std::min(saved.page,pageCount()-1);
    positionDirty_=false;
    lastPositionSync_=std::chrono::steady_clock::now();
  }
  void persistPosition(bool force=false) const {
    if(!resumeEnabled_ || currentSpine_<0)return;
    positionDirty_=true;
    const auto now=std::chrono::steady_clock::now();
    if(!force && lastPositionSync_.time_since_epoch().count()!=0 &&
       now-lastPositionSync_<std::chrono::seconds(5))return;
    if(crosspod::saveReadingPosition(bookPath_,{currentSpine_,currentPage_})) {
      lastPositionSync_=now;
      positionDirty_=false;
    }
  }
  void flushPosition(bool force=true) const {
    if(positionDirty_)persistPosition(force);
  }
  bool configureTypography(crosspod::Typography requested) {
    if(!crosspod::validTypography(requested))return false;
    if(requested.family==typography_.family && requested.size==typography_.size)return true;
    const auto previous=typography_;
    const int spine=currentSpine_;
    const size_t page=currentPage_;
    const auto offset=upstreamSection_?
      upstreamSection_->getVisibleTextOffsetForPage(
        static_cast<uint16_t>(currentPage_)):std::nullopt;
    if(!useTypography(requested)) {
      typographyNotice_="Font file not installed";
      return false;
    }
    if(spine>=0 && (!loadSpine(spine,1) || currentSpine_!=spine)) {
      (void)useTypography(previous);
      (void)loadSpine(spine,1);
      typographyNotice_="Could not repaginate";
      return false;
    }
    if(spine>=0 && pageCount()) {
      if(upstreamSection_ && offset) {
        while(upstreamSection_->isBuilding() &&
              !upstreamSection_->buildReachedVisibleTextOffset(*offset)) {
          if(!upstreamSection_->buildSomeMore(2))break;
        }
        currentPage_=upstreamSection_->getPageForVisibleTextOffset(*offset)
            .value_or(static_cast<uint16_t>(std::min(page,pageCount()-1)));
      } else currentPage_=std::min(page,pageCount()-1);
    }
    if(!crosspod::saveTypography(typography_))
      typographyNotice_="Settings not saved to SD";
    else typographyNotice_.clear();
    persistPosition();
    return true;
  }
  crosspod::Typography typography() const {return typography_;}
  crosspod::ReaderPreferences preferences() const {return preferences_;}
  size_t chapterCount() const {
    if(upstreamEpub_ && upstreamEpub_->getTocItemsCount()>0)
      return static_cast<size_t>(upstreamEpub_->getTocItemsCount());
    return static_cast<size_t>(metadata_.getSpineCount());
  }
  int currentChapter() const {
    if(upstreamEpub_ && upstreamEpub_->getTocItemsCount()>0 &&
       upstreamSection_) {
      const int item=upstreamSection_->getTocIndexForPage(
        static_cast<int>(currentPage_));
      if(item>=0 && item<upstreamEpub_->getTocItemsCount())return item;
    }
    return currentSpine_;
  }
  size_t currentPage() const {return currentPage_;}
  std::string chapterName(size_t index) {
    if(index>=chapterCount())return {};
    if(upstreamEpub_ && upstreamEpub_->getTocItemsCount()>0)
      return upstreamEpub_->getTocItem(static_cast<int>(index)).title;
    const auto entry=metadata_.getSpineEntry(static_cast<int>(index));
    std::string title=entry.href;
    const size_t slash=title.find_last_of('/');
    if(slash!=std::string::npos)title.erase(0,slash+1);
    const size_t ext=title.find_last_of('.');
    if(ext!=std::string::npos)title.resize(ext);
    return title;
  }
  bool goToChapter(int chapter) {
    if(chapter<0||static_cast<size_t>(chapter)>=chapterCount())return false;
    if(upstreamEpub_ && upstreamEpub_->getTocItemsCount()>0) {
      const auto item=upstreamEpub_->getTocItem(chapter);
      if(item.spineIndex<0)return false;
      if(!loadSpine(item.spineIndex,1))return false;
      if(upstreamSection_ && !item.anchor.empty()) {
        auto target=upstreamSection_->findAnchor(item.anchor);
        while(!target && upstreamSection_->isBuilding()) {
          if(!upstreamSection_->buildSomeMore(2))break;
          target=upstreamSection_->findAnchor(item.anchor);
        }
        if(target && *target<pageCount())currentPage_=*target;
      }
    } else if(!loadSpine(chapter,1))return false;
    persistPosition();return true;
  }
  void dumpToc() {
    if(!upstreamEpub_)return;
    const int count=upstreamEpub_->getTocItemsCount();
    std::printf("CrossPod TOC entries: %d\n",count);
    for(int i=0;i<count;++i) {
      const auto t=upstreamEpub_->getTocItem(i);
      std::printf("toc=%d spine=%d depth=%u title=%s anchor=%s\n",
          i,t.spineIndex,t.level,t.title.c_str(),t.anchor.c_str());
    }
  }
  bool rememberPage() {
    const uint32_t offset=upstreamSection_?
      upstreamSection_->getVisibleTextOffsetForPage(
        static_cast<uint16_t>(currentPage_)).value_or(0):0;
    const bool saved=crosspod::addBookmark(bookPath_,
      {currentSpine_,currentPage_,offset});
    typographyNotice_=saved?"Bookmark saved":"Cannot save bookmark";
    return saved;
  }
  std::vector<crosspod::Bookmark> bookmarks() const {
    return crosspod::loadBookmarks(bookPath_);
  }
  bool goToBookmark(const crosspod::Bookmark& mark) {
    // Bookmark chapter numbers are OPF spine indices, not TOC entry ordinals.
    if(mark.spine<0 || !loadSpine(mark.spine,1))return false;
    if(upstreamSection_ && mark.textOffset) {
      while(upstreamSection_->isBuilding() &&
           !upstreamSection_->buildReachedVisibleTextOffset(mark.textOffset)) {
        if(!upstreamSection_->buildSomeMore(2))break;
      }
      currentPage_=upstreamSection_->getPageForVisibleTextOffset(
        mark.textOffset).value_or(static_cast<uint16_t>(
          std::min(mark.page,pageCount()-1)));
    } else currentPage_=std::min(mark.page,pageCount()-1);
    persistPosition();
    return true;
  }
  void showNavigationMenu(bool showingBookmarks,int selected) {
    render();
    renderer_.fillRect(9,38,357,255,true);
    renderer_.fillRect(11,40,353,251,false);
    renderer_.drawText(1,23,48,showingBookmarks?"Bookmarks":"Chapters");
    const auto marks=showingBookmarks?bookmarks():
      std::vector<crosspod::Bookmark>{};
    const size_t count=showingBookmarks?marks.size():chapterCount();
    if(!count)renderer_.drawText(1,25,127,"No bookmarks yet");
    const int start=selected<0?0:(selected/7)*7;
    for(int row=0;row<7 && start+row<static_cast<int>(count);++row) {
      const int item=start+row;
      const int y=81+row*25;
      const bool active=item==selected;
      if(active)renderer_.fillRect(17,y-3,341,22,true);
      std::string label;
      if(showingBookmarks) {
        const auto& mark=marks[item];
        label="Ch "+std::to_string(mark.spine+1)+
          " - Page "+std::to_string(mark.page+1);
      } else label=std::to_string(item+1)+". "+chapterName(item);
      label=clippedLabel(label,34);
      renderer_.drawText(1,24,y,label.c_str(),!active);
    }
    renderer_.drawText(1,23,265,"Wheel: select  Center: go");
    renderer_.drawText(1,23,286,"Back: reading settings");
    display_.displayBuffer();
  }

  bool adjustPreference(int option) {
    crosspod::ReaderPreferences next=preferences_;
    switch(option) {
      case 2: next.linePercent=next.linePercent==100?120:
          (next.linePercent==120?140:100);break;
      case 3: next.margin=next.margin==14?22:
          (next.margin==22?30:14);break;
      case 4: next.alignment=1-next.alignment;break;
      case 5: next.hyphenation=!next.hyphenation;break;
      case 6: next.embeddedStyle=!next.embeddedStyle;break;
      case 7: next.nightMode=!next.nightMode;break;
      default:return false;
    }
    if(!crosspod::validReaderPreferences(next))return false;
    if(option==7) {
      // Inversion is a framebuffer setting; no new Section layout and no
      // SD cache invalidation are necessary.
      preferences_=next;
      clearRasterCache();
      const bool saved=crosspod::saveReaderPreferences(preferences_);
      typographyNotice_=saved?"":"Reading settings not saved";
      return true;
    }
    const auto previous=preferences_;
    const int spine=currentSpine_;
    const size_t priorPage=currentPage_;
    auto offset=upstreamSection_?
      upstreamSection_->getVisibleTextOffsetForPage(
        static_cast<uint16_t>(currentPage_)):std::nullopt;
    preferences_=next;
    clearRasterCache();
    if(spine>=0 && (!loadSpine(spine,1)||currentSpine_!=spine)) {
      preferences_=previous;
      clearRasterCache();
      (void)loadSpine(spine,1);
      currentPage_=std::min(priorPage,pageCount()-1);
      typographyNotice_="Could not update page layout";
      return false;
    }
    // Preserve content position when a completed Section gives an exact
    // visible-text offset; do not assume the old page number is stable after
    // changing margins or line spacing.
    if(upstreamSection_ && offset) {
      while(upstreamSection_->isBuilding() &&
            !upstreamSection_->buildReachedVisibleTextOffset(*offset)) {
        if(!upstreamSection_->buildSomeMore(2))break;
      }
      auto nextPage=upstreamSection_->getPageForVisibleTextOffset(*offset);
      currentPage_=nextPage.value_or(
        static_cast<uint16_t>(std::min(priorPage,pageCount()-1)));
    } else if(pageCount())currentPage_=std::min(priorPage,pageCount()-1);
    if(!crosspod::saveReaderPreferences(preferences_))
      typographyNotice_="Reading settings not saved";
    else typographyNotice_.clear();
    persistPosition();
    return true;
  }
  // Built-in 12pt font ID 2 stays constant when the reader font changes.
  // Scale it for compact menus without loading different SD-card glyph sizes.
  // Crisp fixed 5x7 pixel glyphs; no bilinear resampling or SD font I/O.
  // 2x integer scale makes 10x14 glyphs and fixed 12px character advance.
  // This typography is isolated from the configurable EPUB reading font.
  void drawMenuText(int x,int top,const char* str,int scale=2) {
    static constexpr uint8_t glyphs[95][5]={
    {0x00,0x00,0x00,0x00,0x00}, // 32
    {0x00,0x00,0x5f,0x00,0x00}, // 33
    {0x00,0x07,0x00,0x07,0x00}, // 34
    {0x12,0x3f,0x12,0x3f,0x12}, // 35
    {0x12,0x15,0x1f,0x05,0x08}, // 36
    {0x13,0x0b,0x04,0x1a,0x19}, // 37
    {0x36,0x49,0x55,0x2a,0x50}, // 38
    {0x00,0x04,0x03,0x00,0x00}, // 39
    {0x00,0x1c,0x22,0x41,0x00}, // 40
    {0x00,0x41,0x22,0x1c,0x00}, // 41
    {0x2a,0x1c,0x3e,0x1c,0x2a}, // 42
    {0x08,0x08,0x3e,0x08,0x08}, // 43
    {0x00,0x40,0x30,0x10,0x00}, // 44
    {0x08,0x08,0x08,0x08,0x08}, // 45
    {0x00,0x60,0x60,0x00,0x00}, // 46
    {0x40,0x30,0x08,0x06,0x01}, // 47
    {0x3e,0x51,0x4d,0x43,0x3e}, // 48
    {0x00,0x42,0x7f,0x40,0x00}, // 49
    {0x42,0x61,0x51,0x49,0x46}, // 50
    {0x41,0x49,0x49,0x49,0x36}, // 51
    {0x18,0x14,0x12,0x7f,0x10}, // 52
    {0x4f,0x49,0x49,0x49,0x31}, // 53
    {0x3e,0x49,0x49,0x49,0x30}, // 54
    {0x01,0x71,0x09,0x05,0x03}, // 55
    {0x36,0x49,0x49,0x49,0x36}, // 56
    {0x06,0x49,0x49,0x49,0x3e}, // 57
    {0x00,0x36,0x36,0x00,0x00}, // 58
    {0x00,0x56,0x36,0x00,0x00}, // 59
    {0x08,0x14,0x22,0x41,0x00}, // 60
    {0x0a,0x0a,0x0a,0x0a,0x0a}, // 61
    {0x00,0x41,0x22,0x14,0x08}, // 62
    {0x02,0x01,0x51,0x09,0x06}, // 63
    {0x3e,0x41,0x5d,0x55,0x5e}, // 64
    {0x7e,0x09,0x09,0x09,0x7e}, // 65
    {0x7f,0x49,0x49,0x49,0x36}, // 66
    {0x3e,0x41,0x41,0x41,0x41}, // 67
    {0x7f,0x41,0x41,0x41,0x3e}, // 68
    {0x7f,0x49,0x49,0x49,0x41}, // 69
    {0x7f,0x09,0x09,0x09,0x01}, // 70
    {0x3e,0x41,0x49,0x49,0x79}, // 71
    {0x7f,0x08,0x08,0x08,0x7f}, // 72
    {0x00,0x41,0x7f,0x41,0x00}, // 73
    {0x30,0x40,0x41,0x3f,0x01}, // 74
    {0x7f,0x08,0x14,0x22,0x41}, // 75
    {0x7f,0x40,0x40,0x40,0x40}, // 76
    {0x7f,0x02,0x0c,0x02,0x7f}, // 77
    {0x7f,0x02,0x04,0x08,0x7f}, // 78
    {0x3e,0x41,0x41,0x41,0x3e}, // 79
    {0x7f,0x09,0x09,0x09,0x06}, // 80
    {0x3e,0x41,0x51,0x21,0x5e}, // 81
    {0x7f,0x09,0x19,0x29,0x46}, // 82
    {0x46,0x49,0x49,0x49,0x31}, // 83
    {0x01,0x01,0x7f,0x01,0x01}, // 84
    {0x3f,0x40,0x40,0x40,0x3f}, // 85
    {0x1f,0x20,0x40,0x20,0x1f}, // 86
    {0x3f,0x40,0x38,0x40,0x3f}, // 87
    {0x63,0x14,0x08,0x14,0x63}, // 88
    {0x03,0x04,0x78,0x04,0x03}, // 89
    {0x61,0x51,0x49,0x45,0x43}, // 90
    {0x00,0x7f,0x41,0x41,0x00}, // 91
    {0x01,0x06,0x08,0x30,0x40}, // 92
    {0x00,0x41,0x41,0x7f,0x00}, // 93
    {0x04,0x02,0x01,0x02,0x04}, // 94
    {0x40,0x40,0x40,0x40,0x40}, // 95
    {0x00,0x01,0x02,0x04,0x00}, // 96
    {0x20,0x54,0x54,0x54,0x78}, // 97
    {0x7f,0x44,0x44,0x44,0x38}, // 98
    {0x38,0x44,0x44,0x44,0x44}, // 99
    {0x38,0x44,0x44,0x44,0x7f}, // 100
    {0x38,0x54,0x54,0x54,0x58}, // 101
    {0x08,0x7e,0x09,0x01,0x02}, // 102
    {0x0c,0x52,0x52,0x52,0x3e}, // 103
    {0x7f,0x04,0x04,0x04,0x78}, // 104
    {0x00,0x44,0x7d,0x40,0x00}, // 105
    {0x20,0x40,0x44,0x3d,0x00}, // 106
    {0x7f,0x10,0x28,0x44,0x00}, // 107
    {0x00,0x41,0x7f,0x40,0x00}, // 108
    {0x7c,0x04,0x78,0x04,0x78}, // 109
    {0x7c,0x04,0x04,0x04,0x78}, // 110
    {0x38,0x44,0x44,0x44,0x38}, // 111
    {0x7c,0x14,0x14,0x14,0x08}, // 112
    {0x08,0x14,0x14,0x14,0x7c}, // 113
    {0x7c,0x08,0x04,0x04,0x08}, // 114
    {0x48,0x54,0x54,0x54,0x24}, // 115
    {0x04,0x3f,0x44,0x40,0x20}, // 116
    {0x3c,0x40,0x40,0x20,0x7c}, // 117
    {0x1c,0x20,0x40,0x20,0x1c}, // 118
    {0x3c,0x40,0x30,0x40,0x3c}, // 119
    {0x44,0x28,0x10,0x28,0x44}, // 120
    {0x0c,0x50,0x50,0x50,0x3c}, // 121
    {0x44,0x64,0x54,0x4c,0x44}, // 122
    {0x00,0x08,0x36,0x41,0x00}, // 123
    {0x00,0x00,0x7f,0x00,0x00}, // 124
    {0x00,0x41,0x36,0x08,0x00}, // 125
    {0x04,0x02,0x04,0x04,0x02}, // 126
    };
    for(const unsigned char* p=
          reinterpret_cast<const unsigned char*>(str);*p;++p) {
      const unsigned cp=*p>=32 && *p<=126?*p:static_cast<unsigned>('?');
      if(x+6*scale>352)break;
      const uint8_t* glyph=glyphs[cp-32];
      for(int gx=0;gx<5;++gx)
        for(int gy=0;gy<7;++gy)
          if(glyph[gx] & (1u<<gy))
            renderer_.fillRect(x+gx*scale,top+gy*scale,scale,scale,true);
      x+=6*scale;
    }
  }
  void invertMenuRect(int x,int y,int w,int h) {
    uint8_t* fb=display_.getFrameBuffer();
    if(!fb)return;
    for(int py=std::max(0,y);py<std::min(320,y+h);++py)
      for(int px=std::max(0,x);px<std::min(375,x+w);++px)
        fb[py*HalDisplay::DISPLAY_WIDTH_BYTES+px/8]^=
          static_cast<uint8_t>(0x80u>>(px&7));
  }
  void showTypographyMenu(int selected) {
    renderer_.clearScreen(0xff); // Do not reload and render the EPUB on each wheel step.
    // The Q2 has a 360x320 logical viewport. Keep the heading and footer
    // outside a bounded list; never paint eleven oversized rows at once.
    constexpr int kItems=11;
    constexpr int kRows=9;
    constexpr int kRowTop=73;
    constexpr int kRowHeight=21;
    const int first=std::min(std::max(0,selected-kRows+1),kItems-kRows);
    renderer_.fillRect(10,37,355,258,true);
    renderer_.fillRect(12,39,351,254,false);
    drawMenuText(22,47,"Reading settings");
    renderer_.fillRect(18,66,337,1,true);
    const char* labels[]={"Font","Size","Line spacing","Margins","Alignment",
        "Hyphenation","Book CSS","Night mode","Add bookmark","Bookmarks","Chapters"};
    const std::string values[]={
      crosspod::typographyFamilyName(typography_.family),
      std::to_string(typography_.size)+"pt",
      std::to_string(preferences_.linePercent)+"%",
      std::to_string(preferences_.margin)+"px",
      preferences_.alignment?"Justify":"Book default",
      preferences_.hyphenation?"On":"Off",
      preferences_.embeddedStyle?"On":"Off",
      preferences_.nightMode?"On":"Off",
      "Save page","Saved pages","Browse book"
    };
    for(int row=0;row<kRows && first+row<kItems;++row) {
      const int item=first+row;
      const int y=kRowTop+row*kRowHeight;
      const bool active=selected==item;
      // Invert the completed row, including the glyph pixels, after drawing.
      // Separate columns prevent long values from colliding with labels.
      const std::string label=clippedLabel(labels[item],12);
      const std::string value=clippedLabel(values[item],12);
      drawMenuText(24,y+3,label.c_str());
      drawMenuText(192,y+3,value.c_str());
      if(active)invertMenuRect(18,y,337,kRowHeight);
    }
    renderer_.fillRect(18,267,337,1,true);
    const std::string position=std::to_string(selected+1)+"/"+
      std::to_string(kItems)+"  Wheel: scroll  Center: select";
    drawMenuText(23,277,clippedLabel(position,48).c_str(),1);
    if(!typographyNotice_.empty()) {
      const std::string notice=clippedLabel(typographyNotice_,46);
      drawMenuText(23,292,clippedLabel(notice,48).c_str(),1);
    }
    display_.displayBuffer();
  }
  bool loadSpine(int index,int direction) {
    const int oldSpine=currentSpine_;
    const size_t oldPage=currentPage_;
    auto oldPages=std::move(pages_);
    auto oldSection=std::move(upstreamSection_);
    const int n=metadata_.getSpineCount();
    while(index>=0 && index<n) {
      const auto entry=metadata_.getSpineEntry(index);
      if(upstreamEpub_ && index<upstreamEpub_->getSpineItemsCount()) {
        auto section=std::make_unique<Section>(upstreamEpub_,index,renderer_);
        const ReaderRenderSpec spec=upstreamSpec();
        // Restore complete/partial pages immediately. On a cache miss
        // build only until the first page is available; the event loop will
        // grow the original Section in bounded increments between inputs.
        const bool cached=section->loadSectionFile(spec);
        bool usable=cached && section->pageCount>0;
        if(!cached && section->startBuild(spec)) {
          while(section->isBuilding() && section->pageCount==0) {
            if(!section->buildSomeMore(1))break;
          }
          usable=section->pageCount>0;
        } else if(cached && section->isPartial() && section->startBuild(spec)) {
          // Partial's cached pages remain immediately readable while the
          // next incremental pass rebuilds its page LUT in the background.
          usable=true;
        }
        if(usable) {
          // Reading backwards across a spine requires its *actual* last
          // page, never the partial watermark.
          if(direction<0 && section->isBuilding()) {
            if(!section->buildSomeMore(0))usable=false;
          }
        }
        if(usable) {
          upstreamSection_=std::move(section);
          currentSpine_=index;
          currentPage_=direction<0?upstreamSection_->pageCount-1:0;
          pages_.clear();
          std::printf("CrossPod original Section: spine %d/%d, %zu page(s)%s\n",
              index+1,n,pageCount(),
              upstreamSection_->isBuilding()?" (incremental)":"");
          return true;
        }
        std::fprintf(stderr,
           "CrossPod: original Section could not lay out %s; falling back\n",
           entry.href.c_str());
      }
      std::vector<Paragraph> paragraphs;
      if(parseChapter(archive_,bookPath_,entry.href,paragraphs) &&
         paginate(paragraphs)) {
        upstreamSection_.reset();
        currentSpine_=index;
        currentPage_=direction<0?pages_.size()-1:0;
        std::printf("CrossPod legacy fallback: spine %d/%d, %zu page(s)\n",
            index+1,n,pages_.size());
        return true;
      }
      index+=direction;
    }
    pages_=std::move(oldPages);
    upstreamSection_=std::move(oldSection);
    currentPage_=oldPage;
    currentSpine_=oldSpine;
    return false;
  }
  bool buildNextPages(int maxPages=1) {
    if(!upstreamSection_ || !upstreamSection_->isBuilding())return false;
    const size_t before=pageCount();
    if(!upstreamSection_->buildSomeMore(maxPages)) {
      std::fprintf(stderr,"CrossPod: incremental Section failed; preserving book via fallback\n");
      const int spine=currentSpine_;
      const size_t page=currentPage_;
      upstreamEpub_.reset();
      upstreamSection_.reset();
      if(loadSpine(spine,1)) {
        currentPage_=std::min(page,pageCount()-1);
        persistPosition();
      }
      return true;
    }
    if(!upstreamSection_->isBuilding())
      std::printf("CrossPod Section completed: %zu pages\n",pageCount());
    return pageCount()!=before;
  }
  bool isBuilding() const {return upstreamSection_ && upstreamSection_->isBuilding();}
  size_t estimatedPages() const {
    return upstreamSection_?upstreamSection_->estimatedTotalPages():pageCount();
  }
  bool move(int direction) {
    if(direction>0 && currentPage_+1>=pageCount() && isBuilding()) {
      (void)buildNextPages(2);
      // No new page available yet: remain in this chapter until the
      // incremental parser finishes. Never silently skip unread content.
      if(isBuilding() && currentPage_+1>=pageCount())return false;
    }
    if (direction>0 && currentPage_+1<pageCount()) { ++currentPage_; persistPosition(); return true; }
    if (direction<0 && currentPage_>0) { --currentPage_; persistPosition(); return true; }
    const bool changed=loadSpine(currentSpine_+direction,direction);
    if(changed)persistPosition();
    return changed;
  }
  void applyNightMode() {
    if(!preferences_.nightMode)return;
    auto* buffer=display_.getFrameBuffer();
    if(!buffer)return;
    for(size_t i=0;i<display_.getBufferSize();++i)
      buffer[i]=static_cast<uint8_t>(~buffer[i]);
  }
  void drawProgress() {
    const int totalChapters=metadata_.getSpineCount();
    if(totalChapters<=0 || currentSpine_<0)return;
    const size_t localCount=std::max<size_t>(1,estimatedPages());
    const float local=static_cast<float>(currentPage_+1)/localCount;
    const float progress=std::clamp(
      (static_cast<float>(currentSpine_)+local)/totalChapters,0.0f,1.0f);
    const int filled=static_cast<int>(progress*347.0f);
    renderer_.drawLine(14,314,361,314,true);
    if(filled>0)renderer_.fillRect(14,312,filled,3,true);
  }
  void cacheRenderedPage() {
    const uint8_t* framebuffer=display_.getFrameBuffer();
    if(!framebuffer || currentSpine_<0)return;
    auto& slot=rasterCache_[nextRasterSlot_++%rasterCache_.size()];
    slot.spine=currentSpine_;
    slot.page=currentPage_;
    slot.pixels.assign(framebuffer,framebuffer+HalDisplay::BUFFER_SIZE);
  }
  void render() {
    uint8_t* framebuffer=display_.getFrameBuffer();
    constexpr size_t kBytes=HalDisplay::BUFFER_SIZE;
    if(framebuffer) {
      for(const auto& cached:rasterCache_)
        if(cached.spine==currentSpine_ && cached.page==currentPage_ &&
           cached.pixels.size()==kBytes) {
          std::memcpy(framebuffer,cached.pixels.data(),kBytes);
          return;
        }
    }
    renderer_.clearScreen(0xff);
    if(upstreamSection_) {
      auto page=upstreamSection_->loadPage(static_cast<int>(currentPage_));
      if(page)page->render(renderer_,upstreamFontId_,preferences_.margin,TOP);
      drawProgress();
      applyNightMode();
      cacheRenderedPage();
      return;
    }
    const auto& page=pages_[currentPage_];
    for (const auto& line:page.lines)
      line.block->render(renderer_,1,preferences_.margin,line.y);
    // 4x4 ordered Bayer halftone; preserve image detail on Q2's 1-bit surface.
    static constexpr uint8_t bayer[4][4]={
      {0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    for(const auto& image:page.images) {
      for(int dy=0;dy<image.height;++dy) {
        const size_t sy=static_cast<size_t>(dy)*image.png.height/image.height;
        for(int dx=0;dx<image.width;++dx) {
          const size_t sx=static_cast<size_t>(dx)*image.png.width/image.width;
          const unsigned gray=image.png.gray[sy*image.png.width+sx];
          const unsigned threshold=static_cast<unsigned>(bayer[dy&3][dx&3])*16u+8u;
          if(gray<threshold)renderer_.drawPixel(image.x+dx+(preferences_.margin-14),image.y+dy,true);
        }
      }
    }
    drawProgress();
    applyNightMode();
    cacheRenderedPage();
  }
  void present() {
    render();
    display_.displayBuffer();
  }
  bool savePgm(const std::string& path) {
    render();
    auto* pixels=display_.getFrameBuffer();
    std::FILE* file=std::fopen(path.c_str(),"wb");
    if(!file) return false;
    bool ok=std::fprintf(file,"P5\n375 320\n255\n")>0;
    for (int y=0;ok && y<SCREEN_H;++y) {
      for (int x=0;x<375;++x) {
        bool paper=(pixels[y*47+x/8]&(0x80u>>(x&7)))!=0;
        const unsigned char c=paper?255:0;
        if(std::fwrite(&c,1,1,file)!=1){ok=false;break;}
      }
    }
    if(std::fclose(file)!=0)ok=false;
    return ok;
  }
  size_t imageCount() {
    if(upstreamSection_) {
      size_t result=0;
      for(int i=0;i<upstreamSection_->pageCount;++i) {
        auto page=upstreamSection_->loadPage(i);
        if(page)for(const auto& element:page->elements)
          if(element->getTag()==TAG_PageImage)++result;
      }
      return result;
    }
    size_t n=0;for(const auto& page:pages_)n+=page.images.size();
    return n;
  }
  void report() const {
    std::printf("CrossPod validated %s: page %zu/%zu, spine %d/%d\n",
        bookPath_.c_str(),currentPage_+1,pageCount(),
        currentSpine_+1,metadata_.getSpineCount());
  }
};


// Q2 Pod card-app launcher: the card executable receives no book-path argv.
// The Books shelf is intentionally independent of the stock reader and only
// discovers regular, non-hidden EPUB files from the SD card (one subdir deep).
struct ShelfEntry {
  std::string path;
  std::string label;
};
std::vector<ShelfEntry> scanCardBooks(const std::string& root) {
  std::vector<ShelfEntry> books;
  constexpr size_t MAX_CARD_BOOKS = 512;
  const auto visit = [&](const std::string& dir, bool nested) {
    // Iterative one-level walk: avoid recursion through attacker-controlled
    // directory trees on removable media.
    DIR* d = opendir(dir.c_str());
    if (!d) return std::vector<std::string>{};
    std::vector<std::string> subfolders;
    for (dirent* entry; (entry=readdir(d)) != nullptr;) {
      if (entry->d_name[0] == '.') continue;
      const std::string name(entry->d_name);
      if (name.size() > 240) continue;
      const std::string path=dir+"/"+name;
      if (path.size()>1000) continue;
      struct stat st{};
      if (lstat(path.c_str(),&st) != 0) continue;
      if (S_ISDIR(st.st_mode)) {
        if (!nested && subfolders.size()<128) subfolders.push_back(path);
        continue;
      }
      if (!S_ISREG(st.st_mode) || books.size()>=MAX_CARD_BOOKS) continue;
      if (name.size()<6)continue;
      const std::string_view ending(name.data()+name.size()-5,5);
      if (!(ending==".epub"||ending==".EPUB"||ending==".Epub")) continue;
      books.push_back({path,name.substr(0,name.size()-5)});
    }
    closedir(d);
    return subfolders;
  };
  const auto folders=visit(root,false);
  for(const auto& dir:folders) {
    if(books.size()>=MAX_CARD_BOOKS)break;
    (void)visit(dir,true);
  }
  const auto less=[](const ShelfEntry& a,const ShelfEntry& b){
    // ASCII case-fold only for stable sort; retain Unicode bytes intact.
    const std::string& x=a.label;
    const std::string& y=b.label;
    const size_t n=std::min(x.size(),y.size());
    for(size_t i=0;i<n;++i) {
      const unsigned char cx=static_cast<unsigned char>(x[i]);
      const unsigned char cy=static_cast<unsigned char>(y[i]);
      const auto lx=static_cast<unsigned char>(std::tolower(cx));
      const auto ly=static_cast<unsigned char>(std::tolower(cy));
      if(lx!=ly)return lx<ly;
    }
    return x.size()<y.size() || (x.size()==y.size() && a.path<b.path);
  };
  std::sort(books.begin(),books.end(),less);
  return books;
}
std::string clippedLabel(const std::string& label,size_t maxCodepoints) {
  std::string out;
  size_t pos=0,count=0;
  while(pos<label.size() && count<maxCodepoints) {
    unsigned char c=static_cast<unsigned char>(label[pos]);
    size_t len=c<0x80?1:(c>=0xC2&&c<0xE0?2:(c<0xF0?3:4));
    if(pos+len>label.size())break;
    out.append(label,pos,len);
    pos+=len;
    ++count;
  }
  if(pos<label.size())out+="...";
  return out;
}

class CardShelf {
  const std::vector<ShelfEntry>& books_;
  HalDisplay display_;
  GfxRenderer renderer_;
  EpdFont font_;
  crosspod::ScriptFontFallback scriptFonts_;
  FontDecompressor decompressor_;
  FontCacheManager fontCache_;
  size_t selected_ = 0;
  bool browsing_ = false;
  int homeSelection_ = 0;
  std::string notice_;
  static constexpr int TOP_Y=80, ROW_H=38, VISIBLE_ROWS=5;

 public:
  explicit CardShelf(const std::vector<ShelfEntry>& books,size_t selection=0):
      books_(books),renderer_(display_),font_(&notosans_12_regular),
      fontCache_(renderer_.getFontMap(),renderer_.getSdCardFonts(),renderer_.getTtfFonts()) {
    if(!books_.empty())selected_=std::min(selection,books_.size()-1);
  }
  bool open() {
    display_.begin();
    renderer_.setOrientation(GfxRenderer::LandscapeCounterClockwise);
    renderer_.begin();
    renderer_.insertFont(1,EpdFontFamily(12,&font_));
    // All scripts in EPUB filenames use the same SD font fallback machinery.
    scriptFonts_.load(renderer_,12,{1});
    if(!decompressor_.init())return false;
    fontCache_.setFontDecompressor(&decompressor_);
    renderer_.setFontCacheManager(&fontCache_);
    return true;
  }
  size_t selected() const {return selected_;}
  bool empty() const {return books_.empty();}
  bool browsing() const {return browsing_;}
  bool browseChosen() const {return !browsing_ && (homeSelection_==1 || books_.empty());}
  void enterBrowser() {browsing_=true;notice_.clear();}
  void goHome() {browsing_=false;homeSelection_=0;notice_.clear();}
  const std::string& bookPath() const {return books_[selected_].path;}
  bool change(int direction) {
    notice_.clear();
    if(!browsing_) {
      const int next=std::clamp(homeSelection_+direction,0,1);
      if(next==homeSelection_)return false;
      homeSelection_=next;
      return true;
    }
    if(books_.empty())return false;
    const auto before=selected_;
    if(direction>0 && selected_+1<books_.size())++selected_;
    else if(direction<0 && selected_>0)--selected_;
    return selected_!=before;
  }
  bool tap(int x,int y) {
    if(x<0||x>=375)return false;
    if(!browsing_) {
      if(y>=158&&y<195)homeSelection_=0;
      else if(y>=204&&y<241)homeSelection_=1;
      else return false;
      return true;
    }
    if(books_.empty()||y<TOP_Y||y>=TOP_Y+VISIBLE_ROWS*ROW_H)return false;
    const size_t start=(selected_/VISIBLE_ROWS)*VISIBLE_ROWS;
    const size_t index=start+static_cast<size_t>((y-TOP_Y)/ROW_H);
    if(index>=books_.size())return false;
    selected_=index;
    return true;
  }
  void setNotice(std::string notice) {notice_=std::move(notice);}
  // Draw compact ASCII interface labels using CrossPoint's existing
  // glyph scaler. Reader/body fonts and multilingual fallbacks are unchanged.
  void drawCompactLabel(int x,int baseline,const char* label,int scale=192) {
    for(const unsigned char* cp=
          reinterpret_cast<const unsigned char*>(label);*cp;++cp) {
      if(*cp>=0x80)continue; // these fixed UI labels are ASCII
      x+=renderer_.drawScaledCodepoint(1,*cp,
          EpdFontFamily::REGULAR,x,baseline,scale);
    }
  }
  void paint() {
    renderer_.clearScreen(0xff);
    // Fixed title and footer are outside both the home and file-list regions.
    // Draw at 75% of the native 12pt font, then invert only the header
    // raster so the compact text stays white against the dark title bar.
    drawCompactLabel(16,23,"CrossPod");
    uint8_t* fb=display_.getFrameBuffer();
    for(int y=0;y<39;++y)
      for(int byte=0;byte<HalDisplay::DISPLAY_WIDTH_BYTES;++byte)
        fb[y*HalDisplay::DISPLAY_WIDTH_BYTES+byte]^=0xff;
    if(!browsing_) {
      drawCompactLabel(16,61,"Home");
      renderer_.drawLine(15,74,359,74,true);
      // A compact current-book panel, inspired by CrossPoint's home page.
      // Do not invent book-cover artwork or draw filenames outside the panel.
      renderer_.drawText(1,65,88,"Current book");
      renderer_.drawLine(17,92,52,92,true);
      renderer_.drawLine(17,92,17,138,true);
      renderer_.drawLine(52,92,52,138,true);
      renderer_.drawLine(17,138,52,138,true);
      renderer_.drawLine(22,100,46,100,true);
      renderer_.drawLine(22,106,46,106,true);
      if(books_.empty())
        renderer_.drawText(1,65,119,"No books yet");
      else
        renderer_.drawText(1,65,119,
            clippedLabel(books_[selected_].label,20).c_str());
      renderer_.drawLine(15,149,359,149,true);
      if(!books_.empty()) {
        if(homeSelection_==0)renderer_.fillRect(12,158,350,37,true);
        renderer_.drawText(1,22,165,"Continue reading",homeSelection_!=0);
      }
      if(homeSelection_==1 || books_.empty())
        renderer_.fillRect(12,204,350,37,true);
      renderer_.drawText(1,22,211,"Browse files",
          homeSelection_!=1 && !books_.empty());
    } else {
      char count[64];
      std::snprintf(count,sizeof(count),"Browse files (%zu)",books_.size());
      drawCompactLabel(16,61,count);
      renderer_.drawLine(15,72,359,72,true);
      if(books_.empty()) {
        renderer_.drawText(1,16,102,"No EPUB books found");
        renderer_.drawText(1,16,144,"Copy EPUBs to /Books");
      } else {
        const size_t start=(selected_/VISIBLE_ROWS)*VISIBLE_ROWS;
        for(size_t row=0;row<VISIBLE_ROWS && start+row<books_.size();++row) {
          const size_t index=start+row;
          const int y=TOP_Y+static_cast<int>(row)*ROW_H;
          const bool active=index==selected_;
          if(active)renderer_.fillRect(12,y,350,36,true);
          else renderer_.drawLine(15,y+36,359,y+36,true);
          const std::string label=clippedLabel(books_[index].label,22);
          renderer_.drawText(1,22,y+7,label.c_str(),!active);
        }
      }
    }
    // A single footer line; previous "Wheel: choose" at y=261 overlapped
    // the dark footer and became partly invisible on the actual LCD.
    renderer_.fillRect(0,269,375,51,true);
    const std::string footer=!notice_.empty()
      ?clippedLabel(notice_,24)
      :browsing_?"OK: open   Back: home":"OK: open   Back: exit";
    renderer_.drawText(1,22,280,footer.c_str(),false);
  }
  void present() {paint();display_.displayBuffer();}
  bool savePgm(const std::string& out) {
    paint();
    const uint8_t* pixels=display_.getFrameBuffer();
    std::FILE* f=std::fopen(out.c_str(),"wb");
    if(!f)return false;
    bool ok=std::fprintf(f,"P5\n375 320\n255\n")>0;
    for(int y=0;y<320 && ok;++y)
      for(int x=0;x<375;++x) {
        const unsigned char color=
          (pixels[y*47+x/8]&(0x80u>>(x&7)))?255:0;
        if(std::fwrite(&color,1,1,f)!=1) {ok=false;break;}
      }
    if(std::fclose(f)!=0)ok=false;
    return ok;
  }
};

int runCardApp(const std::string& bookDir) {
  const std::vector<ShelfEntry> books=scanCardBooks(bookDir);
  crosspod::Q2SocketInput input;
  if(!input.open()) {
    std::fputs("CrossPod card: cannot bind /tmp/q2-crosspod.input.sock\n",stderr);
    return 5;
  }
  std::signal(SIGINT,exitOnSignal);
  std::signal(SIGTERM,exitOnSignal);
  size_t selection=0;
  std::unique_ptr<CardShelf> shelf(new CardShelf(books,selection));
  if(!shelf->open())return 4;
  shelf->present();
  crosspod::Q2Controls controls;
  std::unique_ptr<Reader> reader;
  bool typeMenu=false;
  bool navigationMenu=false;
  bool showingBookmarks=false;
  int navigationSelection=0;
  int typeSelection=0;
  auto nextBuildTick=std::chrono::steady_clock::now();
  while(!interrupted) {
    crosspod::PhysicalEvent event{};
    if(!input.receive(event)) {
      if(reader)reader->flushPosition(false);
      const auto now=std::chrono::steady_clock::now();
      if(reader && !typeMenu && !navigationMenu && reader->isBuilding() &&
         now>=nextBuildTick) {
        // One small Section step, no framebuffer refresh: growing the page
        // cache must not flash the current page or monopolize the click wheel.
        (void)reader->buildNextPages(2);
        nextBuildTick=std::chrono::steady_clock::now()+
                      std::chrono::milliseconds(90);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    const auto action=controls.translate(event).action;
    nextBuildTick=std::chrono::steady_clock::now()+
                  std::chrono::milliseconds(300);
    if(reader) {
      if(navigationMenu) {
        const int count=static_cast<int>(showingBookmarks?
          reader->bookmarks().size():reader->chapterCount());
        if(action==crosspod::LogicalAction::Back) {
          navigationMenu=false;
          reader->showTypographyMenu(typeSelection);
        } else if(action==crosspod::LogicalAction::Next ||
                  action==crosspod::LogicalAction::Previous) {
          if(count)navigationSelection=std::clamp(
            navigationSelection+(action==crosspod::LogicalAction::Next?1:-1),
            0,count-1);
          reader->showNavigationMenu(showingBookmarks,navigationSelection);
        } else if(action==crosspod::LogicalAction::Confirm ||
                  event.kind==crosspod::PhysicalKind::TouchUp) {
          if(event.kind==crosspod::PhysicalKind::TouchUp)
            navigationSelection=std::clamp(
              (navigationSelection/7)*7+(event.y-81)/25,
              0,std::max(0,count-1));
          bool changed=false;
          if(count && showingBookmarks) {
            const auto marks=reader->bookmarks();
            if(navigationSelection<static_cast<int>(marks.size()))
              changed=reader->goToBookmark(marks[navigationSelection]);
          } else if(count)changed=reader->goToChapter(navigationSelection);
          if(changed) {
            navigationMenu=false;
            typeMenu=false;
            reader->present();
          } else reader->showNavigationMenu(showingBookmarks,navigationSelection);
        }
        continue;
      }
      if(typeMenu) {
        if(action==crosspod::LogicalAction::Back) {
          typeMenu=false;
          reader->present();
        } else if(action==crosspod::LogicalAction::Previous ||
                  action==crosspod::LogicalAction::Next) {
          typeSelection=(typeSelection+
              (action==crosspod::LogicalAction::Next?1:10))%11;
          reader->showTypographyMenu(typeSelection);
        } else if(action==crosspod::LogicalAction::Confirm ||
                  event.kind==crosspod::PhysicalKind::TouchUp) {
          if(event.kind==crosspod::PhysicalKind::TouchUp)
            typeSelection=std::clamp(std::min(10,std::max(0,typeSelection-8))+
                (event.y-73)/21,0,10);
          if(typeSelection==8) {
            (void)reader->rememberPage();
          } else if(typeSelection>=9) {
            showingBookmarks=typeSelection==9;
            navigationMenu=true;
            navigationSelection=showingBookmarks?0:reader->currentChapter();
          } else if(typeSelection>=2)(void)reader->adjustPreference(typeSelection);
          else {
            auto next=reader->typography();
            if(typeSelection==0) {
              next.family=next.family==crosspod::Typography::Family::Sans?
                crosspod::Typography::Family::Serif:crosspod::Typography::Family::Sans;
            } else next.size=12; // Only 12pt fonts are verified in the Q2 package.
            (void)reader->configureTypography(next);
          }
          if(navigationMenu)
            reader->showNavigationMenu(showingBookmarks,navigationSelection);
          else reader->showTypographyMenu(typeSelection);
        }
        continue;
      }
      if(action==crosspod::LogicalAction::Confirm ||
         (event.kind==crosspod::PhysicalKind::TouchUp && event.y<45)) {
        typeMenu=true;
        typeSelection=0;
        reader->showTypographyMenu(typeSelection);
        continue;
      }
      if(action==crosspod::LogicalAction::Back) {
        reader.reset(); // close book/cache BEFORE reopening the card shelf
        shelf.reset(new CardShelf(books,selection));
        if(!shelf->open())return 6;
        shelf->present();
        continue;
      }
      bool changed=false;
      if(action==crosspod::LogicalAction::Next) changed=reader->move(1);
      if(action==crosspod::LogicalAction::Previous)changed=reader->move(-1);
      if(event.kind==crosspod::PhysicalKind::TouchUp)
        changed=reader->move(event.x>=187?1:-1);
      if(changed)reader->present();
      continue;
    }
    if(action==crosspod::LogicalAction::Back) {
      if(shelf->browsing()) {shelf->goHome();shelf->present();continue;}
      break;
    }
    if(action==crosspod::LogicalAction::Previous && shelf->change(-1))shelf->present();
    if(action==crosspod::LogicalAction::Next && shelf->change(1))shelf->present();
    bool activate=action==crosspod::LogicalAction::Confirm;
    if(event.kind==crosspod::PhysicalKind::TouchUp)activate=shelf->tap(event.x,event.y);
    if(!activate)continue;
    if(shelf->browseChosen()) {
      shelf->enterBrowser();
      shelf->present();
      continue;
    }
    if(shelf->empty())continue;
    selection=shelf->selected();
    const std::string chosen=shelf->bookPath();
    shelf->setNotice("Opening EPUB...");
    shelf->present();
    auto next=std::make_unique<Reader>(chosen);
    if(!next->open()) {
      shelf->setNotice("Cannot read this EPUB");
      shelf->present();
    } else {
      shelf.reset();
      reader=std::move(next);
      reader->enableResume();
      reader->present();
    }
  }
  input.close();
  return 0;
}

void usage() {
  std::fputs(
    "Usage:\n"
    "  crosspod-reader --validate <book.epub>       (no framebuffer)\n"
    "  crosspod-reader --walk <book.epub>           (test pagination)\n"
    "  crosspod-reader --bench <book.epub>          (latency & memory)\n"
    "  crosspod-reader --list-books <directory>     (card scanner test)\n"
    "  crosspod-reader --card-preview <dir> <out.pgm> (menu raster test)\n"
    "  crosspod-reader (no args)                    (Q2 card app)\n"
    "  crosspod-reader --render <book.epub> <page.pgm> (no framebuffer)\n"
    "  crosspod-reader <book.epub>                  (Q2 Pod Books handoff)\n"
    "On Q2, only run interactively after Q2 Pod yields /dev/fb0.\n",stderr);
}
} // namespace

int main(int argc,char** argv) {
  // Card apps start with NO book argument; the on-device picker owns launch.
  if(argc==1) return runCardApp("/mnt/mmc/Books");
  if(argc==3 && std::strcmp(argv[1],"--list-books")==0) {
    const auto books=scanCardBooks(argv[2]);
    for(const auto& b:books)std::printf("%s\n",b.path.c_str());
    return 0;
  }
  if(argc==3 && std::strcmp(argv[1],"--inspect-toc")==0) {
    Reader reader(argv[2]);
    if(!reader.open())return 3;
    reader.dumpToc();
    return 0;
  }
  if(argc==4 && std::strcmp(argv[1],"--render-images")==0) {
    Reader reader(argv[2]);
    if(!reader.open())return 3;
    if(reader.imageCount()==0)return 4;
    std::printf("CrossPod rendered images: %zu\n",reader.imageCount());
    return reader.savePgm(argv[3])?0:5;
  }
  if(argc==4 && std::strcmp(argv[1],"--inspect-styles")==0) {
    // ZipFile retains a const std::string&: the path must outlive the archive.
    const std::string epubPath(argv[2]);
    ZipFile zip(epubPath);
    if(!zip.loadAllFileStatSlims())return 3;
    std::vector<Paragraph> paragraphs;
    if(!parseChapter(zip,epubPath,argv[3],paragraphs))return 4;
    for(size_t i=0;i<paragraphs.size();++i) {
      const auto& p=paragraphs[i];
      bool bold=false,italic=false;
      std::string content;
      for(const auto& run:p.runs) {
        bold=bold||((run.style&EpdFontFamily::BOLD)!=0);
        italic=italic||((run.style&EpdFontFamily::ITALIC)!=0);
        content+=run.text;
      }
      std::printf("paragraph=%zu align=%d rtl=%d bold=%d italic=%d mt=%d mb=%d ml=%d mr=%d indent=%d break_before=%d break_after=%d text=%s\n",
        i,static_cast<int>(p.align),p.rtl,bold,italic,
        p.block.marginTop,p.block.marginBottom,p.block.marginLeft,
        p.block.marginRight,p.block.textIndent,
        p.block.pageBreakBefore,p.block.pageBreakAfter,content.c_str());
    }
    return 0;
  }
  if(argc==4 && std::strcmp(argv[1],"--card-preview")==0) {
    const auto books=scanCardBooks(argv[2]);
    CardShelf shelf(books);
    return shelf.open() && shelf.savePgm(argv[3])?0:4;
  }
  bool dry=false,render=false,walk=false,benchmark=false;
  std::string path,output;
  if(argc==3 && std::strcmp(argv[1],"--validate")==0) {
    dry=true;path=argv[2];
  } else if(argc==3 && std::strcmp(argv[1],"--walk")==0) {
    dry=true;walk=true;path=argv[2];
  } else if(argc==3 && std::strcmp(argv[1],"--bench")==0) {
    dry=true;benchmark=true;path=argv[2];
  } else if(argc==4 && std::strcmp(argv[1],"--render")==0) {
    dry=true;render=true;path=argv[2];output=argv[3];
  } else if (argc==2 && argv[1][0] != '-') {
    path=argv[1];
  } else { usage();return 2; }
  if (path.size()<5 || path.substr(path.size()-5)!=".epub") {
    std::fputs("CrossPod: only EPUB is supported in this device preview\n",stderr);
    return 2;
  }
  const auto openStart=std::chrono::steady_clock::now();
  Reader reader(std::move(path));
  if (!reader.open()) {
    std::fputs("CrossPod: EPUB not supported by current reader slice\n",stderr);
    return 3;
  }
  if(dry) {
    if(benchmark) {
      const auto started=std::chrono::steady_clock::now();
      size_t advanced=0;
      constexpr size_t SAMPLE_PAGES=64;
      while(advanced<SAMPLE_PAGES) {
        if(reader.move(1)) {++advanced;continue;}
        if(!reader.isBuilding())break;
        (void)reader.buildNextPages(4);
      }
      const auto elapsed=std::chrono::steady_clock::now();
      struct rusage usage{};
      (void)getrusage(RUSAGE_SELF,&usage);
      const auto openMs=std::chrono::duration_cast<std::chrono::milliseconds>(
        started-openStart).count();
      const auto turnMs=std::chrono::duration_cast<std::chrono::milliseconds>(
        elapsed-started).count();
      std::printf("CrossPod benchmark: open_ms=%lld sample_pages=%zu walk_ms=%lld maxrss_kb=%ld\n",
        static_cast<long long>(openMs),advanced,
        static_cast<long long>(turnMs),usage.ru_maxrss);
      return 0;
    }
    if(walk) {
      // Exercise real page/spine transitions and the backward boundary.
      // Limit iterations defensively for corrupt EPUBs.
      size_t forward=0,backward=0;
      while(forward<65536) {
        if(reader.move(1)) {++forward;continue;}
        // A page may not have been produced by the latest incremental
        // parse chunk; don't mistake its temporary watermark for EOF.
        if(reader.isBuilding()) {
          (void)reader.buildNextPages(4);
          continue;
        }
        break;
      }
      if(forward>=65536)return 7;
      while(backward<65536 && reader.move(-1))++backward;
      if(backward>=65536||forward!=backward)return 8;
      std::printf("CrossPod walked %zu pages forward and %zu backward\n",
          forward,backward);
    }
    reader.report();
    if (render && !reader.savePgm(output)) return 4;
    return 0;
  }
  crosspod::Q2SocketInput input;
  if(!input.open()) {
    std::fputs("CrossPod: input socket unavailable, refusing interactive mode\n",stderr);
    return 5;
  }
  std::signal(SIGINT,exitOnSignal);
  std::signal(SIGTERM,exitOnSignal);
  reader.enableResume();
  reader.present();
  bool typeMenu=false;
  int typeSelection=0;
  crosspod::Q2Controls controls;
  while(!interrupted) {
    crosspod::PhysicalEvent event{};
    if (!input.receive(event)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    auto action=controls.translate(event).action;
    bool changed=false;
    if(typeMenu) {
      if(action==crosspod::LogicalAction::Back) {
        typeMenu=false;
        reader.present();
      } else if(action==crosspod::LogicalAction::Previous ||
                action==crosspod::LogicalAction::Next) {
        typeSelection=(typeSelection+
            (action==crosspod::LogicalAction::Next?1:6))%7;
        reader.showTypographyMenu(typeSelection);
      } else if(action==crosspod::LogicalAction::Confirm ||
                event.kind==crosspod::PhysicalKind::TouchUp) {
        if(event.kind==crosspod::PhysicalKind::TouchUp)
          typeSelection=std::clamp((event.y-73)/21,0,6);
        if(typeSelection>=2)(void)reader.adjustPreference(typeSelection);
        else {
          auto next=reader.typography();
          if(typeSelection==0)
            next.family=next.family==crosspod::Typography::Family::Sans?
              crosspod::Typography::Family::Serif:crosspod::Typography::Family::Sans;
          else next.size=12; // Only 12pt fonts are verified in the Q2 package.
          (void)reader.configureTypography(next);
        }
        reader.showTypographyMenu(typeSelection);
      }
      continue;
    }
    if(action==crosspod::LogicalAction::Confirm ||
       (event.kind==crosspod::PhysicalKind::TouchUp && event.y<45)) {
      typeMenu=true;
      reader.showTypographyMenu(typeSelection);
      continue;
    }
    if(action==crosspod::LogicalAction::Back)break;
    if(action==crosspod::LogicalAction::Next)changed=reader.move(1);
    if(action==crosspod::LogicalAction::Previous)changed=reader.move(-1);
    if(event.kind==crosspod::PhysicalKind::TouchUp) {
      changed=reader.move(event.x>=187?1:-1);
    }
    if(changed)reader.present();
  }
  input.close();
  return 0;
}
