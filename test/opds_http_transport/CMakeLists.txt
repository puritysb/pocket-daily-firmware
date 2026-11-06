add_executable(OpdsHttpTransportTest
  OpdsHttpTransportTest.cpp
  ${REPO_ROOT}/src/network/OpdsHttpTransport.cpp
)
target_include_directories(OpdsHttpTransportTest BEFORE PRIVATE
  stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/network/Opds/include
)
target_link_libraries(OpdsHttpTransportTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(OpdsHttpTransportTest)
