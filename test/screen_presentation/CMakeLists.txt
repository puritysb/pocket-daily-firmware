# Production screen/content presentation controllers and the shared slot,
# compiled against the content presentation fakes (renderer, font system,
# theme) plus fake frame inputs (ScreenFrameFakes in the test source).
add_executable(screen_presentation_test ScreenPresentationTest.cpp
  ${REPO_ROOT}/src/pocket_daily/ScreenPresentation.cpp
  ${REPO_ROOT}/src/pocket_daily/PresentationSlot.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentPresentation.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentTextLayout.cpp)
target_include_directories(screen_presentation_test BEFORE PRIVATE ../content_presentation/stubs
  ${REPO_ROOT}/lib/EpdFont)
target_link_libraries(screen_presentation_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(screen_presentation_test)
