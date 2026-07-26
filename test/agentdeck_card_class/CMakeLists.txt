add_executable(agentdeck_card_class_test
  CardClassTest.cpp
)

target_link_libraries(agentdeck_card_class_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(agentdeck_card_class_test)
