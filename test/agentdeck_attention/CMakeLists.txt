add_executable(agentdeck_attention_test
  AttentionContractTest.cpp
)

target_link_libraries(agentdeck_attention_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(agentdeck_attention_test)
