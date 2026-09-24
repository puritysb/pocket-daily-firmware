add_executable(deck_store_test DeckStoreTest.cpp ${REPO_ROOT}/src/agentdeck/deck_store.cpp)
target_include_directories(deck_store_test BEFORE PRIVATE stubs)
target_link_libraries(deck_store_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(deck_store_test)
