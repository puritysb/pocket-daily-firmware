add_executable(LiveStudioEventsTest
  LiveStudioEventsTest.cpp
  ${REPO_ROOT}/src/pocket_daily/live_studio/LiveStudioEvents.cpp
)

target_include_directories(LiveStudioEventsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(LiveStudioEventsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LiveStudioEventsTest)
