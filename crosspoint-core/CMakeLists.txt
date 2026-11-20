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
)
# Recent upstream CrossPoint Memory.h uses C++20 constrained templates.
set_property(TARGET crosspoint_pagination_objects PROPERTY CXX_STANDARD 20)
set_property(TARGET crosspoint_pagination_objects PROPERTY CXX_STANDARD_REQUIRED ON)
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
    lib/OtShaper/OtPlan.cpp
    lib/OtShaper/OtBuffer.cpp
    lib/OtShaper/OtGpos.cpp
    lib/OtShaper/OtGsub.cpp
    platform/posix/FontAlloc.c
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
    crosspoint_utf8 crosspoint_layout_foundation ZLIB::ZLIB)
set_target_properties(crosspoint_renderer_link_tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(crosspoint_renderer_link_tests PRIVATE -UNDEBUG)
add_test(NAME crosspoint_renderer_real_link COMMAND crosspoint_renderer_link_tests)
