add_subdirectory(${REPO_ROOT}/host ${CMAKE_CURRENT_BINARY_DIR}/host)
add_executable(gfx_host_test GfxHostTest.cpp ContentImageHostTest.cpp HostStorageTest.cpp HostAbiTest.cpp
  LayoutInputsTest.cpp HomePreviewTest.cpp SdFontPageCacheTest.cpp GlyphFallbackTest.cpp InflateReaderTest.cpp
  ${REPO_ROOT}/src/components/themes/StatusBarTitle.cpp)
target_link_libraries(gfx_host_test PRIVATE pdui_host GTest::gtest_main)
gtest_discover_tests(gfx_host_test)
add_executable(gfx_font_fixture_check GfxFontFixtureCheck.cpp)
target_link_libraries(gfx_font_fixture_check PRIVATE pdui_host)
# Compile/link the public header as C, not just as C++ with extern declarations.
add_executable(host_abi_c_check HostAbiCCheck.c)
target_link_libraries(host_abi_c_check PRIVATE pdui_host)
add_test(NAME HostAbi.CHeaderAndLink COMMAND host_abi_c_check)

# Real font-system lifecycle with fixed discovery and read-only asset storage.
add_executable(library_ui_font_test ../library_ui_font/LibraryFontSystemTest.cpp
  ${REPO_ROOT}/src/SdCardFontSystem.cpp ${REPO_ROOT}/src/ReaderFontSizes.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontManager.cpp)
target_include_directories(library_ui_font_test BEFORE PRIVATE ../library_ui_font/stubs
  ${REPO_ROOT}/host/hal ${REPO_ROOT}/lib/hal ${REPO_ROOT}/lib/Epub)
target_link_libraries(library_ui_font_test PRIVATE pdui_host ArduinoJson GTest::gtest_main)
set_source_files_properties(../library_ui_font/LibraryFontSystemTest.cpp PROPERTIES
  COMPILE_OPTIONS -Wno-missing-field-initializers)
gtest_discover_tests(library_ui_font_test)

# Explicit local World12 font artifact check; does not require a mounted reader.
add_executable(gfx_library_ui_glyph_check LibraryUiGlyphTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp)
target_include_directories(gfx_library_ui_glyph_check PRIVATE ${REPO_ROOT}/lib/LibraryIndex)
target_link_libraries(gfx_library_ui_glyph_check PRIVATE pdui_host)
set_source_files_properties(LibraryUiGlyphTest.cpp PROPERTIES
  COMPILE_OPTIONS -Wno-missing-field-initializers)
