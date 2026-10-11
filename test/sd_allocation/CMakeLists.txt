find_package(Python3 REQUIRED COMPONENTS Interpreter)
# Host tests also run before the PlatformIO pre-build patch on clean checkouts.
# Patch an isolated copy, never modify the developer's SDK during a host test.
set(SDK_TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/sdk-fixture")
set(SDK_HEADER "libs/hardware/SDCardManager/include/SDCardManager.h")
configure_file("${REPO_ROOT}/freeink-sdk/${SDK_HEADER}"
  "${SDK_TEST_ROOT}/freeink-sdk/${SDK_HEADER}" COPYONLY)
configure_file("${REPO_ROOT}/scripts/storage_sdk.patch"
  "${SDK_TEST_ROOT}/scripts/storage_sdk.patch" COPYONLY)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${REPO_ROOT}/scripts/patch_storage_sdk.py"
  --project "${SDK_TEST_ROOT}" RESULT_VARIABLE PATCH_RESULT ERROR_VARIABLE PATCH_ERROR
  OUTPUT_QUIET)
if(NOT PATCH_RESULT EQUAL 0)
  message(FATAL_ERROR "Storage SDK test fixture patch failed: ${PATCH_ERROR}")
endif()
add_executable(sd_allocation_test AllocationTest.cpp)
target_include_directories(sd_allocation_test BEFORE PRIVATE stubs
  "${SDK_TEST_ROOT}/freeink-sdk/libs/hardware/SDCardManager/include")
target_link_libraries(sd_allocation_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(sd_allocation_test)
