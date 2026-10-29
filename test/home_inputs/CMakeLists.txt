add_executable(home_inputs_test
  HomeInputsTest.cpp
  ${REPO_ROOT}/src/pocket_daily/home/HomeInputs.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(home_inputs_test BEFORE PRIVATE
  stubs ../reading_sync_ble/stubs
  ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Utf8)
target_link_libraries(home_inputs_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(home_inputs_test)
