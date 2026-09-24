add_executable(content_manifest_test ContentManifestTest.cpp ContentCardTest.cpp ContentImageTest.cpp ContentPathPolicyTest.cpp ContentTextLayoutTest.cpp ContentPageLayoutTest.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentManifest.cpp ${REPO_ROOT}/src/pocket_daily/ContentCard.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentImage.cpp ${REPO_ROOT}/src/pocket_daily/ContentPathPolicy.cpp
  ${REPO_ROOT}/src/pocket_daily/ContentTextLayout.cpp)
target_link_libraries(content_manifest_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(content_manifest_test)
