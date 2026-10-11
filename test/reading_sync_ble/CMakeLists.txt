# Pocket Reading Sync over BLE v1 (docs/reading-sync-ble-v1.md).

set(READING_SYNC_PURE_SOURCES
  ${REPO_ROOT}/src/pocket_daily/nearby_sync/ReadingSyncProtocol.cpp
  ${REPO_ROOT}/src/pocket_daily/nearby_sync/ExchangeWindowPolicy.cpp
  ${REPO_ROOT}/src/pocket_daily/nearby_sync/ReadSyncStats.cpp)

# Record grammar, chunking/CRC, offer reassembly, callback queue, window policy.
add_executable(reading_sync_ble_protocol_test
  ReadingSyncProtocolTest.cpp
  WifiSetupTest.cpp
  ExchangeWindowPolicyTest.cpp
  ReadSyncStatsTest.cpp
  ${READING_SYNC_PURE_SOURCES})
target_link_libraries(reading_sync_ble_protocol_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(reading_sync_ble_protocol_test)

# READ_LIST / OFFER end to end with a fake radio and the fake SD card, through
# the exchange code shared with GET/POST /api/pocket/v1/reading.
add_executable(reading_sync_ble_session_test
  ReadingSyncSessionTest.cpp
  FakeRadio.cpp
  ${READING_SYNC_PURE_SOURCES}
  ${REPO_ROOT}/src/pocket_daily/nearby_sync/NearbySyncRecords.cpp
  ${REPO_ROOT}/src/pocket_daily/nearby_sync/ReadingSyncSession.cpp
  ${REPO_ROOT}/src/pocket_daily/ReadingExchange.cpp
  ${REPO_ROOT}/src/pocket_daily/ReadingProgress.cpp
  ${REPO_ROOT}/src/pocket_daily/ReadingProgressStore.cpp)
target_include_directories(reading_sync_ble_session_test BEFORE PRIVATE stubs ../stubs ${REPO_ROOT}/lib/Memory)
target_link_libraries(reading_sync_ble_session_test PRIVATE crosspoint_test_common ArduinoJson GTest::gtest_main)
gtest_discover_tests(reading_sync_ble_session_test)

add_executable(reading_sync_ble_memory_test
  ExchangeWindowMemoryTest.cpp
  ${REPO_ROOT}/src/pocket_daily/nearby_sync/ExchangeWindowMemory.cpp
  ${REPO_ROOT}/src/pocket_daily/nearby_sync/ExchangeWindowPolicy.cpp)
target_include_directories(reading_sync_ble_memory_test BEFORE PRIVATE memory_stubs)
target_link_libraries(reading_sync_ble_memory_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(reading_sync_ble_memory_test)

add_executable(reading_sync_ble_dev_cycle_test
  DevSleepCycleTest.cpp
  ${REPO_ROOT}/src/pocket_daily/nearby_sync/DevSleepCycle.cpp
  ${REPO_ROOT}/src/pocket_daily/nearby_sync/ReadSyncStats.cpp)
target_compile_definitions(reading_sync_ble_dev_cycle_test PRIVATE ENABLE_DEV_REMOTE_FLASH)
target_include_directories(reading_sync_ble_dev_cycle_test BEFORE PRIVATE stubs ../stubs)
target_link_libraries(reading_sync_ble_dev_cycle_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(reading_sync_ble_dev_cycle_test)
