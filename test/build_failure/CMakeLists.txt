# Last section-build failure record (src/pocket_daily/BuildFailureLog).
add_executable(build_failure_log_test
  BuildFailureLogTest.cpp
  ${REPO_ROOT}/src/pocket_daily/BuildFailureLog.cpp)
target_include_directories(build_failure_log_test BEFORE PRIVATE ../stubs)
target_link_libraries(build_failure_log_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(build_failure_log_test)
