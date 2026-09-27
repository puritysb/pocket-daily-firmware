add_executable(pocket_daily_learning_pack_test
  LearningPackFormatTest.cpp
  DailyWordTest.cpp
  ${REPO_ROOT}/src/pocket_daily/learning_pack_record.cpp
  ${REPO_ROOT}/src/pocket_daily/home/DailyWord.cpp
)

# Fake HalStorage (test/stubs) and a no-op logger; the whole-pack SHA path in
# learning_pack.cpp (mbedtls) is not part of this target.
target_include_directories(pocket_daily_learning_pack_test BEFORE PRIVATE stubs ../stubs ${REPO_ROOT}/lib/Memory)

target_link_libraries(pocket_daily_learning_pack_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(pocket_daily_learning_pack_test)
