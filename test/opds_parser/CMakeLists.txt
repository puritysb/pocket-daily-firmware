add_library(opds_test_expat STATIC
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)
target_include_directories(opds_test_expat PUBLIC ${REPO_ROOT}/lib/expat)
target_compile_definitions(opds_test_expat PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024)
add_executable(OpdsParserTest
  OpdsParserTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp
)
target_include_directories(OpdsParserTest PRIVATE
  stubs
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/XmlParserUtils
)
target_link_libraries(OpdsParserTest PRIVATE crosspoint_test_common GTest::gtest_main opds_test_expat)
gtest_discover_tests(OpdsParserTest)
