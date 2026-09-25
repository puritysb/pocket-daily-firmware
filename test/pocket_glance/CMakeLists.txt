add_executable(pocket_glance_test
  GlanceFormatTest.cpp
  PocketGlanceTest.cpp
  ${REPO_ROOT}/src/pocket_daily/PocketGlance.cpp
  ${REPO_ROOT}/src/pocket_daily/PocketGlanceJson.cpp
  ${REPO_ROOT}/src/pocket_daily/PocketGlanceStore.cpp
)

target_include_directories(pocket_glance_test BEFORE PRIVATE ../stubs ${REPO_ROOT}/lib/Memory)
target_link_libraries(pocket_glance_test PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(pocket_glance_test)
