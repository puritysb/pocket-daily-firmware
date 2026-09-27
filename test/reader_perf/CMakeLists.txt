# Page-turn timing telemetry record and recorder (src/pocket_daily/ReaderPerf).
add_executable(reader_perf_test
  ReaderPerfTest.cpp
  ${REPO_ROOT}/src/pocket_daily/ReaderPerf.cpp)
target_include_directories(reader_perf_test BEFORE PRIVATE stubs ../stubs ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/hal)
target_compile_definitions(reader_perf_test PRIVATE CROSSPOINT_VERSION="1.7.0-test")
target_link_libraries(reader_perf_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(reader_perf_test)
