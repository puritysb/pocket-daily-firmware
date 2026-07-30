add_executable(agentdeck_glance_test
  GlanceFormatTest.cpp
)

target_link_libraries(agentdeck_glance_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(agentdeck_glance_test)
