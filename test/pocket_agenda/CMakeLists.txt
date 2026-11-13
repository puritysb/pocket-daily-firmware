add_executable(pocket_agenda_test PocketAgendaTest.cpp
 ${REPO_ROOT}/src/pocket_daily/PocketAgenda.cpp
 ${REPO_ROOT}/src/pocket_daily/PocketAgendaJson.cpp
 ${REPO_ROOT}/src/pocket_daily/PocketAgendaStore.cpp)
target_include_directories(pocket_agenda_test BEFORE PRIVATE ../stubs ../content_store/stubs ${REPO_ROOT}/lib/Memory)
target_link_libraries(pocket_agenda_test PRIVATE crosspoint_test_common ArduinoJson GTest::gtest_main)
gtest_discover_tests(pocket_agenda_test)
