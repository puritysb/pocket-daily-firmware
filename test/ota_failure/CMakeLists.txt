add_executable(ota_failure_test
  OtaFailureTest.cpp
)

target_link_libraries(ota_failure_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ota_failure_test)
