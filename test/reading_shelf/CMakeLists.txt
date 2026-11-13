add_executable(reading_shelf_test ReadingShelfTest.cpp
  ${REPO_ROOT}/src/pocket_daily/PocketReadingShelf.cpp
  ${REPO_ROOT}/src/pocket_daily/PocketReadingShelfStore.cpp
  ${REPO_ROOT}/src/pocket_daily/PocketReadingShelfJson.cpp)
target_include_directories(reading_shelf_test BEFORE PRIVATE ../stubs ../content_store/stubs ${REPO_ROOT}/lib/Memory)
target_link_libraries(reading_shelf_test PRIVATE crosspoint_test_common ArduinoJson GTest::gtest_main)
gtest_discover_tests(reading_shelf_test)
