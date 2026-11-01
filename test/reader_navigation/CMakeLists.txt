add_executable(reader_navigation_test
  ReaderNavigationTest.cpp
  ${REPO_ROOT}/src/pocket_daily/ReaderNavigation.cpp
  ${REPO_ROOT}/src/CrossPointState.cpp)
target_include_directories(reader_navigation_test BEFORE PRIVATE stubs)
target_link_libraries(reader_navigation_test PRIVATE crosspoint_test_common ArduinoJson GTest::gtest_main)
gtest_discover_tests(reader_navigation_test)
