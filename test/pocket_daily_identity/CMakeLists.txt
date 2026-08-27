add_executable(pocket_daily_identity_test
  PocketDailyIdentityTest.cpp
)

target_link_libraries(pocket_daily_identity_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(pocket_daily_identity_test)
