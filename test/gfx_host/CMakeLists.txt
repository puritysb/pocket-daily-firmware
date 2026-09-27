add_subdirectory(${REPO_ROOT}/host ${CMAKE_CURRENT_BINARY_DIR}/host)
add_executable(gfx_host_test GfxHostTest.cpp ContentImageHostTest.cpp HostStorageTest.cpp HostAbiTest.cpp
  HomePreviewTest.cpp SdFontPageCacheTest.cpp GlyphFallbackTest.cpp PopupFrameTest.cpp InflateReaderTest.cpp
  ${REPO_ROOT}/src/components/themes/PopupFrame.cpp ${REPO_ROOT}/src/components/themes/StatusBarTitle.cpp)
target_link_libraries(gfx_host_test PRIVATE pdui_host GTest::gtest_main)
# Rasterized popup frames (PBM) land beside the test binary for inspection.
target_compile_definitions(gfx_host_test PRIVATE POPUP_FRAME_OUTPUT="${CMAKE_CURRENT_BINARY_DIR}")
gtest_discover_tests(gfx_host_test)
add_executable(gfx_font_fixture_check GfxFontFixtureCheck.cpp)
target_link_libraries(gfx_font_fixture_check PRIVATE pdui_host)
# Compile/link the public header as C, not just as C++ with extern declarations.
add_executable(host_abi_c_check HostAbiCCheck.c)
target_link_libraries(host_abi_c_check PRIVATE pdui_host)
add_test(NAME HostAbi.CHeaderAndLink COMMAND host_abi_c_check)
