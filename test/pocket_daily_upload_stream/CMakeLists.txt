add_executable(pocket_daily_upload_stream_test
  UploadStreamProtocolTest.cpp
  ${REPO_ROOT}/src/pocket_daily/upload_stream_protocol.cpp
)

target_link_libraries(pocket_daily_upload_stream_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(pocket_daily_upload_stream_test)
