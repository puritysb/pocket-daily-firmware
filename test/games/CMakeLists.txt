add_executable(games_tests
  GameModelsTest.cpp
)

target_link_libraries(games_tests PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(games_tests)
