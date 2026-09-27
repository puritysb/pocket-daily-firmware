find_package(ZLIB REQUIRED)

# XPointer cross-compatibility: the firmware's KOReader XPath resolver/generator
# against positions produced by the Pocket Daily app for real EPUBs.
add_executable(reading_progress_xpointer_test
  XPointerCrossCheckTest.cpp
  stubs/EpubHost.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathResolver.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ProgressMapper.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c)
target_include_directories(reading_progress_xpointer_test BEFORE PRIVATE stubs
  ${REPO_ROOT}/lib/KOReaderSync ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/expat
  ${REPO_ROOT}/lib/XmlParserUtils)
target_compile_definitions(reading_progress_xpointer_test PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024
  READING_PROGRESS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  READING_PROGRESS_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}")
# Vendored expat is C and not held to the firmware's -pedantic flags.
set_source_files_properties(${REPO_ROOT}/lib/expat/xmlparse.c ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(reading_progress_xpointer_test PRIVATE crosspoint_test_common ArduinoJson ZLIB::ZLIB
  GTest::gtest_main)
gtest_discover_tests(reading_progress_xpointer_test)

# Records, offers, list JSON and SD storage of the product module.
add_executable(reading_progress_format_test
  ReadingProgressFormatTest.cpp
  ${REPO_ROOT}/src/pocket_daily/ReadingProgress.cpp
  ${REPO_ROOT}/src/pocket_daily/ReadingProgressStore.cpp)
target_include_directories(reading_progress_format_test BEFORE PRIVATE ../stubs ${REPO_ROOT}/lib/Memory)
target_link_libraries(reading_progress_format_test PRIVATE crosspoint_test_common ArduinoJson GTest::gtest_main)
gtest_discover_tests(reading_progress_format_test)
