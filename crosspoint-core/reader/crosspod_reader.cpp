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
  std::string typographyNotice_;
  std::vector<RenderPage> pages_;
  int currentSpine_ = -1;
  size_t currentPage_ = 0;
  bool resumeEnabled_ = false;
  int lineHeight() const { return std::max(27,typography_.size*2); }
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
    spec.viewportWidth=TEXT_WIDTH;
    spec.viewportHeight=BOTTOM-TOP;
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
    if(next) {
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
  ~Reader() { ImageBlock::setExtractor(nullptr,nullptr); }
  bool open() {
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
  }
  void persistPosition() const {
    if(resumeEnabled_)
      (void)crosspod::saveReadingPosition(bookPath_,{currentSpine_,currentPage_});
  }
  bool configureTypography(crosspod::Typography requested) {
    if(!crosspod::validTypography(requested))return false;
    if(requested.family==typography_.family && requested.size==typography_.size)return true;
    const auto previous=typography_;
    const int spine=currentSpine_;
    const size_t page=currentPage_;
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
    if(spine>=0 && pageCount())currentPage_=std::min(page,pageCount()-1);
    if(!crosspod::saveTypography(typography_))
      typographyNotice_="Settings not saved to SD";
    else typographyNotice_.clear();
    persistPosition();
    return true;
  }
  crosspod::Typography typography() const {return typography_;}
  void showTypographyMenu(int selected) {
    render();
    renderer_.fillRect(13,55,349,205,true);
    renderer_.fillRect(15,57,345,201,false);
    renderer_.drawText(1,30,74,"Reading appearance");
    char row[96];
    std::snprintf(row,sizeof(row),"%s Font: %s",
      selected==0?">":" ",crosspod::typographyFamilyName(typography_.family));
    renderer_.drawText(1,31,117,row);
    std::snprintf(row,sizeof(row),"%s Size: %d",
      selected==1?">":" ",typography_.size);
    renderer_.drawText(1,31,152,row);
    renderer_.drawText(1,31,190,"Wheel: select  Center: change");
    renderer_.drawText(1,31,220,"Back: return to book");
    if(!typographyNotice_.empty())
      renderer_.drawText(1,31,245,typographyNotice_.c_str());
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
        if((section->loadSectionFile(spec) ||
            section->createSectionFile(spec)) && section->pageCount>0) {
          upstreamSection_=std::move(section);
          currentSpine_=index;
          currentPage_=direction<0?upstreamSection_->pageCount-1:0;
          pages_.clear();
          std::printf("CrossPod original Section: spine %d/%d, %zu page(s)\n",
              index+1,n,pageCount());
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
  bool move(int direction) {
    if (direction>0 && currentPage_+1<pageCount()) { ++currentPage_; persistPosition(); return true; }
    if (direction<0 && currentPage_>0) { --currentPage_; persistPosition(); return true; }
    const bool changed=loadSpine(currentSpine_+direction,direction);
    if(changed)persistPosition();
    return changed;
  }
  void render() {
    renderer_.clearScreen(0xff);
    if(upstreamSection_) {
      auto page=upstreamSection_->loadPage(static_cast<int>(currentPage_));
      if(page)page->render(renderer_,upstreamFontId_,LEFT,TOP);
      return;
    }
    const auto& page=pages_[currentPage_];
    for (const auto& line:page.lines)
      line.block->render(renderer_,1,LEFT,line.y);
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
          if(gray<threshold)renderer_.drawPixel(image.x+dx,image.y+dy,true);
        }
      }
    }
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
  std::string notice_;
  static constexpr int TOP_Y=59, ROW_H=36, VISIBLE_ROWS=6;

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
  const std::string& bookPath() const {return books_[selected_].path;}
  bool change(int direction) {
    if(books_.empty())return false;
    const auto before=selected_;
    if(direction>0 && selected_+1<books_.size())++selected_;
    else if(direction<0 && selected_>0)--selected_;
    notice_.clear();
    return selected_!=before;
  }
  bool tap(int x,int y) {
    if(books_.empty() || x<0||x>=375 || y<TOP_Y || y>=TOP_Y+VISIBLE_ROWS*ROW_H)return false;
    const size_t start=(selected_/VISIBLE_ROWS)*VISIBLE_ROWS;
    const size_t index=start+static_cast<size_t>((y-TOP_Y)/ROW_H);
    if(index>=books_.size())return false;
    selected_=index;
    return true;
  }
  void setNotice(std::string notice) {notice_=std::move(notice);}
  void paint() {
    renderer_.clearScreen(0xff);
    renderer_.fillRect(0,0,375,43,true);
    renderer_.drawText(1,17,16,"CrossPod",false);
    char counter[80];
    std::snprintf(counter,sizeof(counter),"%zu EPUB book%s",books_.size(),books_.size()==1?"":"s");
    renderer_.drawText(1,18,45,counter);
    if(books_.empty()) {
      renderer_.drawText(1,18,112,"No EPUB books found.");
      renderer_.drawText(1,18,144,"Copy books to /Books/");
    } else {
      const size_t start=(selected_/VISIBLE_ROWS)*VISIBLE_ROWS;
      for(size_t row=0;row<VISIBLE_ROWS && start+row<books_.size();++row) {
        const size_t index=start+row;
        const int y=TOP_Y+static_cast<int>(row)*ROW_H;
        const bool active=index==selected_;
        if(active)renderer_.fillRect(10,y+2,355,31,true);
        else renderer_.drawLine(14,y+ROW_H-3,361,y+ROW_H-3,true);
        const std::string label=clippedLabel(books_[index].label,34);
        renderer_.drawText(1,18,y+10,label.c_str(),!active);
      }
    }
    if(!notice_.empty()) {
      renderer_.fillRect(0,272,375,20,true);
      renderer_.drawText(1,12,277,clippedLabel(notice_,43).c_str(),false);
    }
    renderer_.drawText(1,18,299,"Wheel: select   Center: open   Back: exit");
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
  int typeSelection=0;
  while(!interrupted) {
    crosspod::PhysicalEvent event{};
    if(!input.receive(event)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    const auto action=controls.translate(event).action;
    if(reader) {
      if(typeMenu) {
        if(action==crosspod::LogicalAction::Back) {
          typeMenu=false;
          reader->present();
        } else if(action==crosspod::LogicalAction::Previous ||
                  action==crosspod::LogicalAction::Next) {
          typeSelection=1-typeSelection;
          reader->showTypographyMenu(typeSelection);
        } else if(action==crosspod::LogicalAction::Confirm ||
                  event.kind==crosspod::PhysicalKind::TouchUp) {
          if(event.kind==crosspod::PhysicalKind::TouchUp)
            typeSelection=event.y>=143?1:0;
          auto next=reader->typography();
          if(typeSelection==0) {
            next.family=next.family==crosspod::Typography::Family::Sans?
              crosspod::Typography::Family::Serif:crosspod::Typography::Family::Sans;
          } else next.size=next.size==12?14:(next.size==14?16:(next.size==16?18:12));
          (void)reader->configureTypography(next);
          reader->showTypographyMenu(typeSelection);
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
    if(action==crosspod::LogicalAction::Back)break;
    if(action==crosspod::LogicalAction::Previous && shelf->change(-1))shelf->present();
    if(action==crosspod::LogicalAction::Next && shelf->change(1))shelf->present();
    bool activate=action==crosspod::LogicalAction::Confirm;
    if(event.kind==crosspod::PhysicalKind::TouchUp)activate=shelf->tap(event.x,event.y);
    if(!activate || shelf->empty())continue;
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
  bool dry=false,render=false,walk=false;
  std::string path,output;
  if(argc==3 && std::strcmp(argv[1],"--validate")==0) {
    dry=true;path=argv[2];
  } else if(argc==3 && std::strcmp(argv[1],"--walk")==0) {
    dry=true;walk=true;path=argv[2];
  } else if(argc==4 && std::strcmp(argv[1],"--render")==0) {
    dry=true;render=true;path=argv[2];output=argv[3];
  } else if (argc==2 && argv[1][0] != '-') {
    path=argv[1];
  } else { usage();return 2; }
  if (path.size()<5 || path.substr(path.size()-5)!=".epub") {
    std::fputs("CrossPod: only EPUB is supported in this device preview\n",stderr);
    return 2;
  }
  Reader reader(std::move(path));
  if (!reader.open()) {
    std::fputs("CrossPod: EPUB not supported by current reader slice\n",stderr);
    return 3;
  }
  if(dry) {
    if(walk) {
      // Exercise real page/spine transitions and the backward boundary.
      // Limit iterations defensively for corrupt EPUBs.
      size_t forward=0,backward=0;
      while(forward<65536 && reader.move(1))++forward;
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
        typeSelection=1-typeSelection;
        reader.showTypographyMenu(typeSelection);
      } else if(action==crosspod::LogicalAction::Confirm ||
                event.kind==crosspod::PhysicalKind::TouchUp) {
        if(event.kind==crosspod::PhysicalKind::TouchUp)
          typeSelection=event.y>=143?1:0;
        auto next=reader.typography();
        if(typeSelection==0)
          next.family=next.family==crosspod::Typography::Family::Sans?
            crosspod::Typography::Family::Serif:crosspod::Typography::Family::Sans;
        else next.size=next.size==12?14:(next.size==14?16:(next.size==16?18:12));
        (void)reader.configureTypography(next);
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
