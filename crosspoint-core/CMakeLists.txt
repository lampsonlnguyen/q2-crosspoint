cmake_minimum_required(VERSION 3.16)
project(crosspod_crosspoint_core LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep original upstream CrossPoint UTF-8 code unmodified.
add_library(crosspoint_utf8 STATIC
    lib/Utf8/Utf8.cpp
)
target_include_directories(crosspoint_utf8 PUBLIC lib/Utf8)

enable_testing()
add_executable(crosspoint_core_tests tests/utf8_test.cpp)
target_link_libraries(crosspoint_core_tests PRIVATE crosspoint_utf8)
add_test(NAME crosspoint_unicode COMMAND crosspoint_core_tests)

add_executable(crosspoint_controls_tests tests/controls_test.cpp)
add_test(NAME q2_wheel_touch_mapping COMMAND crosspoint_controls_tests)

add_executable(crosspoint_display_tests tests/display_test.cpp)
add_test(NAME q2_lcd_color_adapter COMMAND crosspoint_display_tests)

# CrossPoint's original TXT-to-XHTML pipeline, compiled unchanged.
# POSIX shims replace its Arduino Print/SDK allocation and filesystem helpers.
add_library(crosspoint_txt STATIC lib/Txt/TxtToHtml.cpp)
target_include_directories(crosspoint_txt PUBLIC
    lib/Txt
    platform/posix
)
add_executable(crosspoint_txt_tests tests/txt_to_html_test.cpp)
target_link_libraries(crosspoint_txt_tests PRIVATE crosspoint_txt)
add_test(NAME crosspoint_vietnamese_txt COMMAND crosspoint_txt_tests)

# The original CrossPoint EPUB container.xml parser. The XML parser itself is
# Expat (system library on Linux), not the ESP32 toolchain or Q2 Pod EPUB code.
find_package(EXPAT REQUIRED)
add_library(crosspoint_epub_container STATIC lib/Epub/parsers/ContainerParser.cpp)
target_include_directories(crosspoint_epub_container PUBLIC
    lib/Epub/parsers
    lib/XmlParserUtils
    platform/posix
)
target_link_libraries(crosspoint_epub_container PUBLIC EXPAT::EXPAT)
add_executable(crosspoint_epub_container_tests tests/container_test.cpp)
target_link_libraries(crosspoint_epub_container_tests PRIVATE crosspoint_epub_container)
add_test(NAME crosspoint_epub_container COMMAND crosspoint_epub_container_tests)

# Q2 Pod keeps its existing Books shelf and supplies the selected path.
# CrossPoint uses this standalone contract rather than the game's app runtime.
add_executable(crosspoint_books_handoff_tests tests/book_handoff_test.cpp)
add_test(NAME q2_books_handoff COMMAND crosspoint_books_handoff_tests)

# Assertions must remain active in Release (-DNDEBUG would otherwise disable all tests).
foreach(t crosspoint_core_tests crosspoint_controls_tests crosspoint_display_tests
          crosspoint_txt_tests crosspoint_epub_container_tests crosspoint_books_handoff_tests)
    target_compile_options(${t} PRIVATE -UNDEBUG)
endforeach()

# CrossPoint ZIP reader: original upstream ZipFile source, Linux storage adapter
# and a bounded zlib streaming replacement for the ESP32 miniz implementation.
find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_library(crosspoint_zip STATIC
    lib/ZipFile/ZipFile.cpp
    platform/posix/InflateStream.cpp
)
target_include_directories(crosspoint_zip PUBLIC
    lib/ZipFile
    platform/posix
)
target_link_libraries(crosspoint_zip PUBLIC ZLIB::ZLIB)

set(EPUB_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/multilingual.epub")
add_custom_command(OUTPUT "${EPUB_FIXTURE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_epub_fixture.py" "${EPUB_FIXTURE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_epub_fixture.py"
    VERBATIM
)
add_custom_target(epub_fixture ALL DEPENDS "${EPUB_FIXTURE}")
add_executable(crosspoint_epub_zip_tests tests/epub_zip_test.cpp)
target_link_libraries(crosspoint_epub_zip_tests PRIVATE crosspoint_zip crosspoint_epub_container)
add_dependencies(crosspoint_epub_zip_tests epub_fixture)
target_compile_options(crosspoint_epub_zip_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_multilingual_epub_zip
    COMMAND crosspoint_epub_zip_tests "${EPUB_FIXTURE}")

# Tiny native/Linux diagnostic executable; not a full graphical reader.
add_executable(crosspod-epub-check tools/epub_check.cpp)
target_link_libraries(crosspod-epub-check PRIVATE crosspoint_zip crosspoint_epub_container)
add_dependencies(crosspod-epub-check epub_fixture)
add_test(NAME crosspoint_epub_check_cli
    COMMAND crosspod-epub-check "${EPUB_FIXTURE}")

# Original CrossPoint OPF manifest and spine parser. Temporary storage and
# metadata-cache callback are POSIX transitional adapters (full cache to port).
add_library(crosspoint_opf STATIC lib/Epub/parsers/ContentOpfParser.cpp)
target_include_directories(crosspoint_opf PUBLIC
    platform/posix
    lib/Epub/parsers
    lib/XmlParserUtils
    lib/Serialization
    lib/Epub
)
target_link_libraries(crosspoint_opf PUBLIC EXPAT::EXPAT)
add_executable(crosspoint_opf_spine_tests tests/opf_spine_test.cpp)
target_link_libraries(crosspoint_opf_spine_tests PRIVATE crosspoint_zip crosspoint_opf)
add_dependencies(crosspoint_opf_spine_tests epub_fixture)
target_compile_options(crosspoint_opf_spine_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_original_opf_spine
    COMMAND crosspoint_opf_spine_tests "${EPUB_FIXTURE}")

# Actual CrossPoint low-RAM on-disk metadata/spine/TOC cache and buffered IO.
add_library(crosspoint_book_cache STATIC
    lib/Epub/Epub/BookMetadataCache.cpp
)
target_include_directories(crosspoint_book_cache PUBLIC
    lib/Epub
    lib/Epub/Epub
    lib/Serialization
    lib/Utf8
    lib/ZipFile
    platform/posix
)
target_link_libraries(crosspoint_book_cache PUBLIC crosspoint_zip crosspoint_utf8)
target_link_libraries(crosspoint_opf PUBLIC crosspoint_book_cache)

# Standalone Linux Q2 Pod input protocol and nonblocking UNIX datagram receiver.
# No AWTK or ESP32 dependencies are needed for this interface.
add_library(crosspod_q2_input STATIC platform/q2/Q2SocketInput.cpp)
target_include_directories(crosspod_q2_input PUBLIC platform/q2)
add_executable(crosspod_q2_input_tests tests/socket_input_test.cpp)
target_link_libraries(crosspod_q2_input_tests PRIVATE crosspod_q2_input)
target_compile_options(crosspod_q2_input_tests PRIVATE -UNDEBUG)
add_test(NAME crosspod_q2_input_protocol COMMAND crosspod_q2_input_tests)

# Linux framebuffer backed by the Q2's two native portrait LCD pages.
# Static ARGB rotation tests do not require access to /dev/fb0.
add_library(crosspod_q2_framebuffer STATIC platform/q2/Q2Framebuffer.cpp)
target_include_directories(crosspod_q2_framebuffer PUBLIC platform/q2)
add_executable(crosspod_framebuffer_tests tests/framebuffer_test.cpp)
target_link_libraries(crosspod_framebuffer_tests PRIVATE crosspod_q2_framebuffer)
target_compile_options(crosspod_framebuffer_tests PRIVATE -UNDEBUG)
add_test(NAME crosspod_q2_framebuffer_rotation COMMAND crosspod_framebuffer_tests)

# Original CrossPoint bounded text arena (WordStore) and stylesheet parser.
# Both compile untouched; only their ESP32 cache reclamation and filesystem
# services are provided through POSIX compatibility interfaces.
add_library(crosspoint_layout_foundation STATIC
    lib/Epub/WordStore.cpp
    lib/Epub/css/CssParser.cpp
)
# The upstream CSS parser marks standard-algorithm comparisons constexpr.
# std::equal is constexpr in C++20, not in the older MIPS GCC C++17 library.
set_property(TARGET crosspoint_layout_foundation PROPERTY CXX_STANDARD 20)
set_property(TARGET crosspoint_layout_foundation PROPERTY CXX_STANDARD_REQUIRED ON)
target_include_directories(crosspoint_layout_foundation PUBLIC
    lib/Epub
    lib/Epub/css
    platform/posix
)
add_executable(crosspoint_layout_foundation_tests tests/layout_foundation_test.cpp)
target_link_libraries(crosspoint_layout_foundation_tests PRIVATE crosspoint_layout_foundation)
target_compile_options(crosspoint_layout_foundation_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_original_css_wordstore
    COMMAND crosspoint_layout_foundation_tests)

# CrossPoint's original Unicode glyph lookup, fixed-point font metrics,
# ligature substitution and font-family style selection.
add_library(crosspoint_font_metrics STATIC
    lib/EpdFont/EpdFont.cpp
    lib/EpdFont/EpdFontFamily.cpp
)
target_include_directories(crosspoint_font_metrics PUBLIC
    lib/EpdFont
    lib/Utf8
)
target_link_libraries(crosspoint_font_metrics PUBLIC crosspoint_utf8)
add_executable(crosspoint_font_metrics_tests tests/font_metrics_test.cpp)
target_link_libraries(crosspoint_font_metrics_tests PRIVATE crosspoint_font_metrics)
target_compile_options(crosspoint_font_metrics_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_unicode_font_metrics COMMAND crosspoint_font_metrics_tests)

# Original CrossPoint Unicode CJK line-breaking, punctuation and hyphenation
# primitives (used by ParsedText / TextBlock when they are ported).
add_library(crosspoint_linebreak STATIC
    lib/Epub/CjkBreaks.cpp
    lib/Epub/hyphenation/HyphenationCommon.cpp
)
target_include_directories(crosspoint_linebreak PUBLIC
    lib/Epub
    lib/Utf8
)
target_link_libraries(crosspoint_linebreak PUBLIC crosspoint_utf8)
add_executable(crosspoint_linebreak_tests tests/cjk_breaks_test.cpp)
target_link_libraries(crosspoint_linebreak_tests PRIVATE crosspoint_linebreak)
target_compile_options(crosspoint_linebreak_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_cjk_linebreak COMMAND crosspoint_linebreak_tests)


# Full original CrossPoint multilingual Liang hyphenator, including its
# generated language tries. The dictionary data is compiled into this target
# rather than replaced by an ASCII-only or Q2 native word splitter.
add_library(crosspoint_hyphenation STATIC
    lib/Epub/hyphenation/Hyphenator.cpp
    lib/Epub/hyphenation/LanguageRegistry.cpp
    lib/Epub/hyphenation/LiangHyphenation.cpp
)
# Original IndicScripts.h uses std::any_of in constexpr helpers.
# Apply C++20 to this target rather than changing u8 literal semantics in tests.
set_property(TARGET crosspoint_hyphenation PROPERTY CXX_STANDARD 20)
set_property(TARGET crosspoint_hyphenation PROPERTY CXX_STANDARD_REQUIRED ON)
target_include_directories(crosspoint_hyphenation PUBLIC
    lib
    lib/Epub
    lib/Epub/hyphenation
    lib/Utf8
)
target_link_libraries(crosspoint_hyphenation PUBLIC crosspoint_linebreak crosspoint_utf8)
add_executable(crosspoint_hyphenator_tests tests/hyphenator_test.cpp)
target_link_libraries(crosspoint_hyphenator_tests PRIVATE crosspoint_hyphenation)
target_compile_options(crosspoint_hyphenator_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_real_liang_hyphenation COMMAND crosspoint_hyphenator_tests)

# End-to-end EPUB ZIP stylesheet stream into CrossPoint's original CSS cascade.
add_executable(crosspoint_epub_css_pipeline_tests tests/epub_css_pipeline_test.cpp)
target_link_libraries(crosspoint_epub_css_pipeline_tests PRIVATE
    crosspoint_zip crosspoint_layout_foundation)
add_dependencies(crosspoint_epub_css_pipeline_tests epub_fixture)
target_compile_options(crosspoint_epub_css_pipeline_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_epub_css_pipeline
    COMMAND crosspoint_epub_css_pipeline_tests "${EPUB_FIXTURE}")

# Original CrossPoint MiniBidi (UAX#9) and Arabic contextual presentation.
# This is used by upstream ParsedText/GfxRenderer. We build unmodified C
# text direction algorithms on Linux rather than a simplified local rewrite.
add_library(crosspoint_bidi STATIC
    lib/MiniBidi/minibidi.c
    lib/MiniBidi/BidiUtils.cpp
)
target_include_directories(crosspoint_bidi PUBLIC
    lib/MiniBidi
    lib/Utf8
    platform/posix
)
target_link_libraries(crosspoint_bidi PUBLIC crosspoint_utf8)
add_executable(crosspoint_bidi_tests tests/bidi_test.cpp)
target_link_libraries(crosspoint_bidi_tests PRIVATE crosspoint_bidi)
target_compile_options(crosspoint_bidi_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_unicode_bidi_arabic COMMAND crosspoint_bidi_tests)

# Original CrossPoint ParsedText line composition/pagination engine and Page
# model. Compile-check only until the complete GfxRenderer and HTML chapter
# parser are connected. Do NOT present these object files as a working reader.
add_library(crosspoint_pagination_objects OBJECT
    lib/Epub/ParsedText.cpp
    lib/Epub/Page.cpp
    lib/Epub/blocks/TextBlock.cpp
)
# Recent upstream CrossPoint Memory.h uses C++20 constrained templates.
set_property(TARGET crosspoint_pagination_objects PROPERTY CXX_STANDARD 20)
set_property(TARGET crosspoint_pagination_objects PROPERTY CXX_STANDARD_REQUIRED ON)
target_compile_definitions(crosspoint_pagination_objects PRIVATE CROSSPOD_LINUX=1)
target_include_directories(crosspoint_pagination_objects PRIVATE
    lib
    lib/Epub
    lib/Epub/css
    lib/EpdFont
    lib/GfxRenderer
    lib/Utf8
    lib/MiniBidi
    lib/Serialization
    lib/XmlParserUtils
    platform/posix
)

# Q2-backed CrossPoint HalDisplay implementation. Upstream uses 1=white,
# 0=black in its byte buffer; the conversion to Q2 LCD color is explicit.
add_library(crosspoint_q2_hal_display STATIC platform/posix/HalDisplay.cpp)
target_include_directories(crosspoint_q2_hal_display PUBLIC
    platform/posix platform/q2)
target_link_libraries(crosspoint_q2_hal_display PUBLIC crosspod_q2_framebuffer)
add_executable(crosspoint_q2_hal_display_tests tests/q2_hal_display_test.cpp)
target_link_libraries(crosspoint_q2_hal_display_tests PRIVATE crosspoint_q2_hal_display)
target_compile_options(crosspoint_q2_hal_display_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_q2_monochrome_hal COMMAND crosspoint_q2_hal_display_tests)

# Compile original upstream renderer as an object library. This is a
# compile-only integration gate: linking a real reader requires all font,
# image and FreeInk-independent Q2 adapters.
add_library(crosspoint_renderer_objects OBJECT
    lib/GfxRenderer/GfxRenderer.cpp
    lib/GfxRenderer/Bitmap.cpp
    lib/GfxRenderer/BitmapHelpers.cpp
    lib/GfxRenderer/FontCacheManager.cpp
    lib/Memory/BuildScratch.cpp
    lib/EpdFont/FontDecompressor.cpp
    lib/EpdFont/ComplexShaper.cpp
    lib/EpdFont/SdCardFont.cpp
    lib/Utf8/IndicReorder.cpp
    lib/OtShaper/OtShaper.cpp
    lib/OtShaper/OtIndic.cpp
    lib/OtShaper/OtNormalize.cpp
    lib/OtShaper/OtUse.cpp
    lib/OtShaper/OtFace.cpp
    lib/OtShaper/OtLayout.cpp
    lib/OtShaper/OtContext.cpp
    lib/OtShaper/OtPlan.cpp
    lib/OtShaper/OtPlanBuild.cpp
    lib/OtShaper/OtBuffer.cpp
    lib/OtShaper/OtGpos.cpp
    lib/OtShaper/OtGsub.cpp
    lib/OtShaper/OtTable.cpp
    platform/posix/FontAlloc.c
    platform/posix/FlashBlobCache.cpp
)
target_compile_definitions(crosspoint_renderer_objects PRIVATE CROSSPOD_LINUX=1)
set_target_properties(crosspoint_renderer_objects PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(crosspoint_renderer_objects PRIVATE
    lib/EpdFont/scripts
    lib/OtShaper
    lib
    lib/Epub
    lib/Epub/css
    lib/EpdFont
    lib/GfxRenderer
    lib/Utf8
    lib/MiniBidi
    lib/Serialization
    lib/Memory
    lib/XmlParserUtils
    platform/posix
    platform/q2
)

# Link gate: force every reference from the original GfxRenderer object through
# the linker. Compile-only success is not enough to establish a working engine.
add_executable(crosspoint_renderer_link_tests
    tests/renderer_link_test.cpp
    $<TARGET_OBJECTS:crosspoint_renderer_objects>)
target_include_directories(crosspoint_renderer_link_tests PRIVATE
    lib lib/Epub lib/EpdFont lib/GfxRenderer lib/Utf8 lib/MiniBidi
    lib/Memory platform/posix)
target_link_libraries(crosspoint_renderer_link_tests PRIVATE
    crosspoint_q2_hal_display crosspoint_font_metrics crosspoint_bidi
    crosspoint_utf8 crosspoint_layout_foundation crosspoint_linebreak ZLIB::ZLIB)
set_target_properties(crosspoint_renderer_link_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(crosspoint_renderer_link_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_renderer_real_link COMMAND crosspoint_renderer_link_tests)

# Validate bounds checking for the original OpenType table reader independently
# of the still-in-progress GfxRenderer full-link integration.
add_executable(crosspoint_ot_table_tests
    tests/ot_table_test.cpp
    lib/OtShaper/OtTable.cpp)
target_include_directories(crosspoint_ot_table_tests PRIVATE lib/OtShaper)
target_compile_options(crosspoint_ot_table_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_opentype_table_bounds COMMAND crosspoint_ot_table_tests)

# Genuine upstream EPUB text-pagination and glyph-rendering integration gate.
# The small XHTML extractor in the test is NOT part of the production reader;
# complete ChapterHtmlSlimParser and CSS/images remain separately required.
add_executable(crosspod_epub_page_preview
    tests/epub_page_preview.cpp
    lib/Epub/ParsedText.cpp
    lib/Epub/blocks/TextBlock.cpp
    $<TARGET_OBJECTS:crosspoint_renderer_objects>)
target_include_directories(crosspod_epub_page_preview PRIVATE
    lib lib/Epub lib/Epub/blocks lib/Epub/css lib/EpdFont lib/GfxRenderer
    lib/Utf8 lib/MiniBidi lib/Memory lib/Serialization platform/posix)
target_compile_definitions(crosspod_epub_page_preview PRIVATE CROSSPOD_LINUX=1)
set_target_properties(crosspod_epub_page_preview PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(crosspod_epub_page_preview PRIVATE
    crosspoint_zip crosspoint_hyphenation crosspoint_linebreak
    crosspoint_q2_hal_display crosspoint_font_metrics crosspoint_bidi
    crosspoint_layout_foundation crosspoint_utf8 EXPAT::EXPAT ZLIB::ZLIB)
target_compile_options(crosspod_epub_page_preview PRIVATE -UNDEBUG)
add_dependencies(crosspod_epub_page_preview epub_fixture)
add_test(NAME crosspoint_real_epub_text_page_preview
    COMMAND crosspod_epub_page_preview "${EPUB_FIXTURE}" "${CMAKE_CURRENT_BINARY_DIR}/crosspod-epub-preview.pgm")

# Separate self-contained original Vietnamese prose EPUB fixture for native
# glyph raster QA. Does not change the existing multilingual test's OPF spine.
set(VIETNAMESE_EPUB_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/vietnamese-prose.epub")
add_custom_command(OUTPUT "${VIETNAMESE_EPUB_FIXTURE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_vietnamese_epub_fixture.py" "${VIETNAMESE_EPUB_FIXTURE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_vietnamese_epub_fixture.py"
    VERBATIM)
add_custom_target(vietnamese_epub_fixture ALL DEPENDS "${VIETNAMESE_EPUB_FIXTURE}")
add_dependencies(crosspod_epub_page_preview vietnamese_epub_fixture)
add_test(NAME crosspoint_vietnamese_book_page_preview
    COMMAND crosspod_epub_page_preview "${VIETNAMESE_EPUB_FIXTURE}" "${CMAKE_CURRENT_BINARY_DIR}/crosspod-vietnamese-preview.pgm" "OEBPS/chapter1.xhtml")

# Native Q2 EPUB vertical slice: original CrossPoint ZIP/OPF/spine, ParsedText,
# TextBlock and full linked GfxRenderer. The POSIX XHTML text-block collector
# remains temporary until ChapterHtmlSlimParser/CSS/images are fully ported.
add_executable(crosspod-reader
    reader/crosspod_reader.cpp
    reader/JpegPreview.c
    platform/posix/EpubRuntime.cpp
    platform/posix/ImageDecoderFactory.cpp
    lib/Epub/converters/ImageDimsProbe.cpp
    lib/Epub/htmlEntities.cpp
    lib/Epub/blocks/ImageBlock.cpp
    lib/Epub/Page.cpp
    lib/Epub/Section.cpp
    lib/Epub/parsers/ChapterHtmlSlimParser.cpp
    lib/Epub/ParsedText.cpp
    lib/Epub/blocks/TextBlock.cpp
    $<TARGET_OBJECTS:crosspoint_renderer_objects>)
# The upstream parser uses Arduino time helpers; POSIX supplies these shims.
set_source_files_properties(lib/Epub/parsers/ChapterHtmlSlimParser.cpp
    lib/Epub/Section.cpp PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/platform/posix/ArduinoCompat.h")
target_include_directories(crosspod-reader PRIVATE
    lib lib/Epub lib/Epub/Epub lib/Epub/parsers lib/Epub/blocks
    lib/Epub/css lib/EpdFont lib/GfxRenderer lib/Utf8 lib/MiniBidi
    lib/Memory lib/Serialization lib/XmlParserUtils reader platform/posix platform/q2)
target_compile_definitions(crosspod-reader PRIVATE CROSSPOD_LINUX=1)
set_target_properties(crosspod-reader PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(crosspod-reader PRIVATE
    crosspoint_zip crosspoint_epub_container crosspoint_opf
    crosspoint_book_cache crosspoint_hyphenation crosspoint_linebreak
    crosspoint_q2_hal_display crosspod_q2_input
    crosspoint_font_metrics crosspoint_bidi
    crosspoint_layout_foundation crosspoint_utf8 EXPAT::EXPAT ZLIB::ZLIB JPEG::JPEG)
add_dependencies(crosspod-reader epub_fixture vietnamese_epub_fixture)
add_test(NAME crosspod_reader_real_epub_spine
    COMMAND crosspod-reader --validate "${EPUB_FIXTURE}")
# Active card reader must preserve CrossPoint CSS alignment, emphasis and RTL.
add_test(NAME crosspod_reader_original_css_cascade
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/css_chapter_reader_test.py"
        "$<TARGET_FILE:crosspod-reader>" "${EPUB_FIXTURE}")
add_test(NAME crosspod_reader_vietnamese_epub
    COMMAND crosspod-reader --render "${VIETNAMESE_EPUB_FIXTURE}"
        "${CMAKE_CURRENT_BINARY_DIR}/crosspod-reader-vietnamese.pgm")

# Exercise real page transitions and backwards traversal without Q2 hardware.
add_test(NAME crosspod_reader_walk_multispine
    COMMAND crosspod-reader --walk "${EPUB_FIXTURE}")
add_test(NAME crosspod_reader_walk_vietnamese
    COMMAND crosspod-reader --walk "${VIETNAMESE_EPUB_FIXTURE}")

# Bounded, atomic per-book reading-position persistence (POSIX Q2 adapter).
add_executable(crosspod_reading_position_tests tests/reading_position_test.cpp)
target_include_directories(crosspod_reading_position_tests PRIVATE reader)
target_compile_options(crosspod_reading_position_tests PRIVATE -UNDEBUG)
add_test(NAME crosspod_reading_position_persistence COMMAND crosspod_reading_position_tests)

# Font-family/size preference serialization and input validation.
add_executable(crosspod_typography_tests tests/typography_test.cpp)
target_include_directories(crosspod_typography_tests PRIVATE reader)
target_compile_options(crosspod_typography_tests PRIVATE -UNDEBUG)
add_test(NAME crosspod_two_font_preferences COMMAND crosspod_typography_tests)

# Preserve original CrossPoint non-visible XHTML filtering semantics.
add_executable(crosspoint_visible_text_tests tests/visible_text_utils_test.cpp)
target_include_directories(crosspoint_visible_text_tests PRIVATE lib/Epub)
target_compile_options(crosspoint_visible_text_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_visible_xhtml_elements COMMAND crosspoint_visible_text_tests)

# Compile-only interface gate for the exact upstream CrossPoint XHTML parser.
# Full implementation still needs Epub/Section, images and CSS reader wiring.
add_executable(crosspoint_chapter_parser_interface_tests
    tests/original_chapter_parser_interface_test.cpp)
target_include_directories(crosspoint_chapter_parser_interface_tests PRIVATE
    lib lib/Epub lib/Epub/Epub lib/EpdFont lib/GfxRenderer
    lib/Utf8 lib/MiniBidi platform/posix)
set_target_properties(crosspoint_chapter_parser_interface_tests PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_test(NAME crosspoint_original_chapter_parser_interface
    COMMAND crosspoint_chapter_parser_interface_tests)

# Card app is the same CrossPoint reader ELF: q2-pod invokes it with argc==1.
# The binary gets its book path from its own scrollable /Books/ picker.
add_test(NAME crosspod_reader_card_empty_library
    COMMAND crosspod-reader --list-books "${CMAKE_CURRENT_BINARY_DIR}/empty-books")
add_test(NAME crosspod_reader_card_list_fixture
    COMMAND crosspod-reader --list-books "${CMAKE_CURRENT_BINARY_DIR}/books-test")

# Bounded PNG decoding with grayscale, alpha and palette input common in EPUB.
add_executable(crosspod_png_formats_tests tests/png_formats_test.cpp)
target_include_directories(crosspod_png_formats_tests PRIVATE reader)
target_link_libraries(crosspod_png_formats_tests PRIVATE ZLIB::ZLIB)
target_compile_options(crosspod_png_formats_tests PRIVATE -UNDEBUG)
add_test(NAME crosspod_png_formats_and_crc COMMAND crosspod_png_formats_tests)

# Alpha 2: illustrated EPUB fixture and actual original ZipFile PNG extraction.
# This is an ingestion gate, not a claim that ImageBlock paints to the LCD.
set(ILLUSTRATED_EPUB_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/illustrated.epub")
add_custom_command(OUTPUT "${ILLUSTRATED_EPUB_FIXTURE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_illustrated_epub_fixture.py" "${ILLUSTRATED_EPUB_FIXTURE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_illustrated_epub_fixture.py"
    VERBATIM)
add_custom_target(illustrated_epub_fixture ALL DEPENDS "${ILLUSTRATED_EPUB_FIXTURE}")
add_executable(crosspod_illustrated_epub_resources
    tests/illustrated_epub_resource_test.cpp)
target_link_libraries(crosspod_illustrated_epub_resources PRIVATE crosspoint_zip)
target_compile_options(crosspod_illustrated_epub_resources PRIVATE -UNDEBUG)
add_dependencies(crosspod_illustrated_epub_resources illustrated_epub_fixture)
add_test(NAME crosspod_original_zip_illustrated_epub
    COMMAND crosspod_illustrated_epub_resources "${ILLUSTRATED_EPUB_FIXTURE}")
# Verify the live XHTML collector can resolve ../styles relative to a nested
# chapter while maintaining image/text sequence. Host CI tests this with both
# GCC and Clang, not merely a separate parser stub.
add_test(NAME crosspod_reader_relative_css_and_inline_image
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/relative_epub_resource_test.py"
      "$<TARGET_FILE:crosspod-reader>" "${ILLUSTRATED_EPUB_FIXTURE}")

# Streaming parser regression: 3 MiB well-formed XHTML, malformed markup,
# and the 4 MiB safety limit. The active reader must process XML incrementally.
add_test(NAME crosspod_reader_bounded_xhtml_streaming
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/xhtml_streaming_test.py"
      "$<TARGET_FILE:crosspod-reader>" "${ILLUSTRATED_EPUB_FIXTURE}")

# End-to-end illustrated EPUB: PNG extracted, decoded, placed and rasterized
# into a 375x320 monochrome CrossPod page.
add_dependencies(crosspod-reader illustrated_epub_fixture)
add_test(NAME crosspod_reader_renders_epub_png
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_illustrated_render.py"
      "$<TARGET_FILE:crosspod-reader>" "${ILLUSTRATED_EPUB_FIXTURE}"
      "${CMAKE_CURRENT_BINARY_DIR}/illustrated-page.pgm")

# Ensure real reader opens and rasterizes both PNG and JPEG inside the EPUB.
add_test(NAME crosspod_reader_png_and_jpeg
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/epub_png_jpeg_test.py"
      "$<TARGET_FILE:crosspod-reader>" "${ILLUSTRATED_EPUB_FIXTURE}"
      "${CMAKE_CURRENT_BINARY_DIR}/jpeg-png-page.pgm")

# Chinese/Japanese EPUB. Q2 hardware build renders this through the original
# CrossPoint Section and verifies CJK glyph fallback is actually used.
set(CJK_EPUB_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/cjk-script.epub")
add_custom_command(OUTPUT "${CJK_EPUB_FIXTURE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_cjk_epub_fixture.py" "${CJK_EPUB_FIXTURE}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_cjk_epub_fixture.py"
    VERBATIM)
add_custom_target(cjk_epub_fixture ALL DEPENDS "${CJK_EPUB_FIXTURE}")
add_dependencies(crosspod-reader cjk_epub_fixture)
add_test(NAME crosspod_reader_cjk_original_section
    COMMAND crosspod-reader --validate "${CJK_EPUB_FIXTURE}")

# Script fixtures: Korean/Hindi are release gates, other upstream scripts
# are parsed as real EPUBs to ensure UTF-8 survives the original Section path.
foreach(script korean devanagari bengali gujarati gurmukhi kannada malayalam
               oriya sinhala tamil telugu arabic hebrew)
    set(script_epub "${CMAKE_CURRENT_BINARY_DIR}/script-${script}.epub")
    add_custom_command(OUTPUT "${script_epub}"
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_multiscript_epub_fixture.py"
        "${script}" "${script_epub}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_multiscript_epub_fixture.py"
      VERBATIM)
    add_custom_target("crosspod_fixture_${script}" ALL DEPENDS "${script_epub}")
    add_dependencies(crosspod-reader "crosspod_fixture_${script}")
    add_test(NAME "crosspod_original_script_${script}"
      COMMAND crosspod-reader --validate "${script_epub}")
endforeach()

# CrossPoint's actual XHTML chapter parser is compiled directly as a gate.
# Compile only until missing FreeInk image codecs and reading activity
# dependencies can be ported; this is not a linked reader.
add_library(crosspoint_xhtml_parser_objects OBJECT
    lib/Epub/parsers/ChapterHtmlSlimParser.cpp
)
target_compile_options(crosspoint_xhtml_parser_objects PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/platform/posix/ArduinoCompat.h)
set_target_properties(crosspoint_xhtml_parser_objects PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(crosspoint_xhtml_parser_objects PRIVATE
    lib
    lib/Epub
    lib/Epub/parsers
    lib/Epub/css
    lib/EpdFont
    lib/GfxRenderer
    lib/Utf8
    lib/MiniBidi
    lib/Serialization
    lib/XmlParserUtils
    platform/posix
    platform/q2
)

# CrossPoint's real section composition logic bridges XHTML parsing,
# ParsedText and Page. Compile it as an object while the remaining
# GfxRenderer/ImageBlock implementations are being linked on the renderer PR.
add_library(crosspoint_section_objects OBJECT
    lib/Epub/Section.cpp
)
target_compile_options(crosspoint_section_objects PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/platform/posix/ArduinoCompat.h)
set_target_properties(crosspoint_section_objects PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(crosspoint_section_objects PRIVATE
    lib
    lib/Epub
    lib/Epub/parsers
    lib/Epub/css
    lib/EpdFont
    lib/GfxRenderer
    lib/Utf8
    lib/MiniBidi
    lib/Serialization
    lib/XmlParserUtils
    platform/posix
)

# Experimental FULL upstream reader link gate. Unlike the compile-only gates
# above this deliberately resolves every reference used by the original
# ChapterHtmlSlimParser, Section and Page. It must pass before switching the
# Q2 user-facing reader to the original section engine.
add_executable(crosspoint_original_engine_link_tests
    tests/original_section_runtime_test.cpp
    lib/Epub/blocks/ImageBlock.cpp
    platform/posix/EpubRuntime.cpp
    platform/posix/ImageDecoderFactory.cpp
    reader/JpegPreview.c
    lib/Epub/converters/ImageDimsProbe.cpp
    lib/Epub/htmlEntities.cpp
    $<TARGET_OBJECTS:crosspoint_xhtml_parser_objects>
    $<TARGET_OBJECTS:crosspoint_section_objects>
    $<TARGET_OBJECTS:crosspoint_pagination_objects>
    $<TARGET_OBJECTS:crosspoint_renderer_objects>)
target_include_directories(crosspoint_original_engine_link_tests PRIVATE
    lib lib/Epub lib/Epub/Epub lib/Epub/parsers lib/Epub/blocks
    lib/Epub/css lib/EpdFont lib/GfxRenderer lib/Utf8 lib/MiniBidi
    lib/Memory lib/Serialization lib/XmlParserUtils reader platform/posix platform/q2)
target_compile_definitions(crosspoint_original_engine_link_tests PRIVATE CROSSPOD_LINUX=1)
set_target_properties(crosspoint_original_engine_link_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(crosspoint_original_engine_link_tests PRIVATE
    crosspoint_zip crosspoint_epub_container crosspoint_opf
    crosspoint_book_cache crosspoint_hyphenation crosspoint_linebreak
    crosspoint_q2_hal_display crosspoint_font_metrics crosspoint_bidi
    crosspoint_layout_foundation crosspoint_utf8 EXPAT::EXPAT ZLIB::ZLIB JPEG::JPEG)
add_dependencies(crosspoint_original_engine_link_tests epub_fixture illustrated_epub_fixture)
target_compile_options(crosspoint_original_engine_link_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_original_engine_real_layout
    COMMAND crosspoint_original_engine_link_tests "${EPUB_FIXTURE}")
