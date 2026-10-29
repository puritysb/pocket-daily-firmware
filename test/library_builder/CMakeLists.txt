add_executable(LibraryBuilderTest
  LibraryBuilderTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryBuilder.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryIndexFile.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryFormat.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(LibraryBuilderTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
)
target_link_libraries(LibraryBuilderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(LibraryBuilderTest)

# Compile the same builder and baseline tests with the development-only budget.
add_executable(LibraryBuilderDevBudgetTest
  LibraryBuilderTest.cpp
  DevBuildBudgetTest.cpp
  LibraryTrialIndexTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryBuilder.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryIndexFile.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryFormat.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(LibraryBuilderDevBudgetTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
)
target_compile_definitions(LibraryBuilderDevBudgetTest PRIVATE ENABLE_DEV_REMOTE_FLASH)
target_link_libraries(LibraryBuilderDevBudgetTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(LibraryBuilderDevBudgetTest TEST_PREFIX "LibraryDev.")
