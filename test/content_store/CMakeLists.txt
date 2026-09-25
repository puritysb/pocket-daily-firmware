add_executable(content_store_test ContentRevisionStoreTest.cpp
  ${REPO_ROOT}/lib/hal/DirectoryAccounting.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentRevisionStore.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentActiveStore.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentViewState.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentSealStore.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentManifest.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentCard.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentImage.cpp)
target_include_directories(content_store_test BEFORE PRIVATE stubs ../stubs ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal)
target_link_libraries(content_store_test PRIVATE crosspoint_test_common GTest::gtest_main)
if(NOT APPLE)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(content_store_test PRIVATE OpenSSL::Crypto)
endif()
gtest_discover_tests(content_store_test)
