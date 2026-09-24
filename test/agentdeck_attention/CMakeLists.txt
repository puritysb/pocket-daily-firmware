add_executable(agentdeck_attention_test
  AttentionContractTest.cpp
  OutboundQueueTest.cpp
)

target_link_libraries(agentdeck_attention_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(agentdeck_attention_test PRIVATE ${REPO_ROOT}/lib/Memory)

gtest_discover_tests(agentdeck_attention_test)
