add_executable(content_presentation_test ContentPresentationTest.cpp ContentSessionInputTest.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentPresentation.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentTextLayout.cpp)
target_include_directories(content_presentation_test BEFORE PRIVATE stubs)
target_link_libraries(content_presentation_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(content_presentation_test)
