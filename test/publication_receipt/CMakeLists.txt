add_executable(publication_receipt_test PublicationReceiptTest.cpp
  ${REPO_ROOT}/src/pocket_daily/PublicationReceipt.cpp)
target_include_directories(publication_receipt_test BEFORE PRIVATE ../stubs ../reading_progress/stubs)
target_link_libraries(publication_receipt_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(publication_receipt_test)
