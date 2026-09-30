add_executable(LiveStudioEventsTest
  LiveStudioEventsTest.cpp
  ${REPO_ROOT}/src/pocket_daily/live_studio/LiveStudioEvents.cpp
  ${REPO_ROOT}/src/pocket_daily/live_studio/NetHealth.cpp
)

target_include_directories(LiveStudioEventsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(LiveStudioEventsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LiveStudioEventsTest)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME SyncRouteBoundaries
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/test_sync_routes.py
)

add_executable(RadioHealthTest
  RadioHealthTest.cpp
  TransferAdmissionTest.cpp
)

target_include_directories(RadioHealthTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(RadioHealthTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RadioHealthTest)
