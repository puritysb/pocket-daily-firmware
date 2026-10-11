add_executable(wifi_setup_store_test WifiStoreTest.cpp ${REPO_ROOT}/src/WifiCredentialStore.cpp)
target_include_directories(wifi_setup_store_test BEFORE PRIVATE stubs ../stubs ${REPO_ROOT}/lib/Serialization)
target_link_libraries(wifi_setup_store_test PRIVATE crosspoint_test_common ArduinoJson GTest::gtest_main)
gtest_discover_tests(wifi_setup_store_test)
