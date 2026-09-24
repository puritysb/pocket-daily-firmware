add_executable(hal_storage_test HalStorageTest.cpp DirectoryAccountingTest.cpp
  ${REPO_ROOT}/lib/hal/HalStorage.cpp ${REPO_ROOT}/lib/hal/DirectoryAccounting.cpp)
target_include_directories(hal_storage_test BEFORE PRIVATE stubs ${REPO_ROOT}/lib/hal)
target_link_libraries(hal_storage_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(hal_storage_test)
