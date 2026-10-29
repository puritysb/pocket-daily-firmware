# Reader layout-ahead cadence and page pipeline (src/activities/reader/ReaderLayoutAhead,
# ReaderPageRenderer) against the real EPUB parser, layout and renderer.
file(GLOB READER_LAYOUT_HYPHENATION ${REPO_ROOT}/lib/Epub/Epub/hyphenation/*.cpp)
set(READER_LAYOUT_FIRMWARE
  ${REPO_ROOT}/src/activities/reader/ReaderLayoutAhead.cpp
  ${REPO_ROOT}/src/activities/reader/ReaderPageRenderer.cpp
  ${REPO_ROOT}/src/pocket_daily/ReaderPerf.cpp
  ${REPO_ROOT}/lib/Epub/Epub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContainerParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNavParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNcxParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${READER_LAYOUT_HYPHENATION}
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c)
add_executable(reader_layout_test ReaderLayoutAheadTest.cpp stubs/ImageStubs.cpp ${REPO_ROOT}/host/HostChecksums.cpp
  ${READER_LAYOUT_FIRMWARE})
# Production sources keep their own warning baseline; the test itself is held to the suite's flags.
# Arduino sources see Arduino.h implicitly (ESP, millis, String): force-include the stub there.
set(READER_LAYOUT_FIRMWARE_CXX ${READER_LAYOUT_FIRMWARE})
list(FILTER READER_LAYOUT_FIRMWARE_CXX EXCLUDE REGEX "\\.c$")
set(READER_LAYOUT_FIRMWARE_C ${READER_LAYOUT_FIRMWARE})
list(FILTER READER_LAYOUT_FIRMWARE_C INCLUDE REGEX "\\.c$")
set_source_files_properties(${READER_LAYOUT_FIRMWARE_CXX} PROPERTIES COMPILE_OPTIONS
  "-w;-include;${CMAKE_CURRENT_SOURCE_DIR}/stubs/Arduino.h")
set_source_files_properties(${READER_LAYOUT_FIRMWARE_C} ${REPO_ROOT}/host/HostChecksums.cpp PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(reader_layout_test BEFORE PRIVATE stubs ${REPO_ROOT}/host/hal ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MiniBidi ${REPO_ROOT}/lib/InflateReader ${REPO_ROOT}/lib/uzlib/src ${REPO_ROOT}/lib/expat
  ${REPO_ROOT}/lib/FsHelpers ${REPO_ROOT}/lib/XmlParserUtils ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/JpegToBmpConverter ${REPO_ROOT}/lib/PngToBmpConverter ${REPO_ROOT}/lib/hal)
target_compile_definitions(reader_layout_test PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024 HAVE_ARC4RANDOM_BUF
  CROSSPOINT_VERSION="reader-layout-test" READER_LAYOUT_EPUB="${REPO_ROOT}/test/epubs/short-spines.epub"
  LONG_PARAGRAPH_EPUB="${REPO_ROOT}/test/reading_progress/fixtures/pocket-daily-epub-check.epub")
find_package(ZLIB REQUIRED)
target_link_libraries(reader_layout_test PRIVATE crosspoint_test_common ZLIB::ZLIB GTest::gtest_main)
gtest_discover_tests(reader_layout_test)

# Exact reading positions: page text offsets from the real layout against the XPointer
# resolvers (lib/KOReaderSync) and the reading-progress fixtures.
add_executable(reading_position_layout_test ReadingPositionLayoutTest.cpp stubs/ImageStubs.cpp
  ${REPO_ROOT}/host/HostChecksums.cpp ${READER_LAYOUT_FIRMWARE} ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathResolver.cpp ${REPO_ROOT}/lib/KOReaderSync/ChapterTextOffsetResolver.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ProgressMapper.cpp)
set_source_files_properties(${REPO_ROOT}/lib/KOReaderSync/ChapterXPathResolver.cpp ${REPO_ROOT}/lib/KOReaderSync/ChapterTextOffsetResolver.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ProgressMapper.cpp PROPERTIES COMPILE_OPTIONS
  "-w;-include;${CMAKE_CURRENT_SOURCE_DIR}/stubs/Arduino.h")
target_include_directories(reading_position_layout_test BEFORE PRIVATE stubs ${REPO_ROOT}/host/hal ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MiniBidi ${REPO_ROOT}/lib/InflateReader ${REPO_ROOT}/lib/uzlib/src ${REPO_ROOT}/lib/expat
  ${REPO_ROOT}/lib/FsHelpers ${REPO_ROOT}/lib/XmlParserUtils ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/JpegToBmpConverter ${REPO_ROOT}/lib/PngToBmpConverter ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/KOReaderSync)
target_compile_definitions(reading_position_layout_test PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024 HAVE_ARC4RANDOM_BUF
  CROSSPOINT_VERSION="reading-position-test"
  READING_PROGRESS_FIXTURES="${REPO_ROOT}/test/reading_progress/fixtures")
target_link_libraries(reading_position_layout_test PRIVATE crosspoint_test_common ArduinoJson ZLIB::ZLIB
  GTest::gtest_main)
gtest_discover_tests(reading_position_layout_test)

foreach(target reader_layout_test reading_position_layout_test)
  target_sources(${target} PRIVATE stubs/HalMemory.cpp)
endforeach()

# Inject allocation failures only into the real ZIP/miniz streaming code. The
# Section/parser/cache pipeline remains the same as the reader-layout target.
set(SECTION_RETRY_FIRMWARE ${READER_LAYOUT_FIRMWARE})
list(REMOVE_ITEM SECTION_RETRY_FIRMWARE ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp)
add_executable(section_inflate_retry_test SectionInflateRetryTest.cpp ZipSmallEntryTest.cpp ZipInflateFaults.cpp
  stubs/ImageStubs.cpp stubs/HalMemory.cpp ${REPO_ROOT}/host/HostChecksums.cpp
  ${SECTION_RETRY_FIRMWARE}
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/FontAlloc.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
get_target_property(READER_LAYOUT_INCLUDES reader_layout_test INCLUDE_DIRECTORIES)
get_target_property(READER_LAYOUT_DEFINITIONS reader_layout_test COMPILE_DEFINITIONS)
target_include_directories(section_inflate_retry_test BEFORE PRIVATE ${READER_LAYOUT_INCLUDES}
  ${REPO_ROOT}/lib/miniz/src ${REPO_ROOT}/lib/miniz/third_party)
target_compile_definitions(section_inflate_retry_test PRIVATE ${READER_LAYOUT_DEFINITIONS})
# Included production implementations have their existing warning baseline.
set_source_files_properties(ZipInflateFaults.cpp PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(section_inflate_retry_test PRIVATE crosspoint_test_common ZLIB::ZLIB GTest::gtest_main)
gtest_discover_tests(section_inflate_retry_test)

add_executable(image_diagnostics_test ImageDiagnosticsTest.cpp ImagePaintDiagnosticsTest.cpp)
target_include_directories(image_diagnostics_test PRIVATE ${REPO_ROOT}/lib/Epub)
target_link_libraries(image_diagnostics_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(image_diagnostics_test)

# Real page/image/font/cache rendering with a deterministic memory-pressure codec.
add_executable(reader_image_memory_test ReaderImageMemoryTest.cpp stubs/HalMemory.cpp
  ${REPO_ROOT}/host/HostChecksums.cpp ${READER_LAYOUT_FIRMWARE}
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/FontAlloc.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
get_target_property(READER_IMAGE_INCLUDES reader_layout_test INCLUDE_DIRECTORIES)
target_include_directories(reader_image_memory_test BEFORE PRIVATE ${READER_IMAGE_INCLUDES}
  ${REPO_ROOT}/lib/miniz/src ${REPO_ROOT}/lib/miniz/third_party)
get_target_property(READER_IMAGE_DEFINITIONS reader_layout_test COMPILE_DEFINITIONS)
target_compile_definitions(reader_image_memory_test PRIVATE ${READER_IMAGE_DEFINITIONS})
target_link_libraries(reader_image_memory_test PRIVATE crosspoint_test_common ZLIB::ZLIB GTest::gtest_main)
gtest_discover_tests(reader_image_memory_test)
