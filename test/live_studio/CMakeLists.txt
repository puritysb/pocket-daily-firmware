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

add_executable(UiPackTest
  UiPackTest.cpp
  ${REPO_ROOT}/src/pocket_daily/live_studio/UiPack.cpp
)

target_include_directories(UiPackTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/open-x4-sdk/libs/display/EInkDisplay/include
)

target_link_libraries(UiPackTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UiPackTest)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME ThemeMetricBindings
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/test_theme_metric_bindings.py
)
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
