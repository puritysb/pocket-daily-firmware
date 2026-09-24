add_executable(sd_font_test SdCardFontTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(sd_font_test BEFORE PRIVATE stubs
  ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/Memory)
target_link_libraries(sd_font_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(sd_font_test)

# Explicit local-artifact check; not a hardware test or a required SD mount.
add_executable(sd_font_fixture_check SdFontFixtureCheck.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(sd_font_fixture_check BEFORE PRIVATE stubs
  ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/Memory)
target_link_libraries(sd_font_fixture_check PRIVATE crosspoint_test_common)
