add_executable(bt_tests
  test_coinshift_types.cpp
  test_coinshift_replay.cpp
  test_coinshift_policy.cpp
  test_coinshift_json.cpp
  test_coinshift_privfile.cpp
  test_coinshift_sim.cpp)
if(BT_WITH_NET)
  target_sources(bt_tests PRIVATE test_coinshift_wallet.cpp)
  target_link_libraries(bt_tests PRIVATE coinshift_node bt_net)
endif()
target_link_libraries(bt_tests PRIVATE bt_test_harness bt_core_test_cases coinshift_core bt_core)
target_compile_definitions(bt_tests PRIVATE BT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME bt_tests COMMAND bt_tests)
