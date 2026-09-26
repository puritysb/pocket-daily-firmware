add_executable(pocket_daily_staged_firmware_test
  StagedFirmwareTest.cpp
  ${REPO_ROOT}/src/pocket_daily/staged_firmware.cpp
)

target_link_libraries(pocket_daily_staged_firmware_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(pocket_daily_staged_firmware_test)
