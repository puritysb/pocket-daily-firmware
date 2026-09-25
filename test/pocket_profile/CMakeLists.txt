add_executable(pocket_profile_test
  PocketProfileTest.cpp
  ${REPO_ROOT}/src/pocket_daily/PocketProfile.cpp
  ${REPO_ROOT}/src/pocket_daily/PocketProfileJson.cpp
  ${REPO_ROOT}/src/pocket_daily/PocketProfileStore.cpp
)

target_include_directories(pocket_profile_test BEFORE PRIVATE ../stubs)
target_link_libraries(pocket_profile_test PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(pocket_profile_test)

add_executable(pocket_preferences_test
  PreferencesUpdateTest.cpp
  ${REPO_ROOT}/src/pocket_daily/web/PreferencesUpdate.cpp
)

target_link_libraries(pocket_preferences_test PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(pocket_preferences_test)
