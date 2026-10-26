# Coinshift (eCash slot 255) rule module and binaries.
add_library(coinshift_core STATIC
  src/types.cpp
  src/net.cpp
  src/state.cpp
  src/fixture.cpp
  src/json.cpp
  src/privfile.cpp
  src/simcheck.cpp)
target_include_directories(coinshift_core PUBLIC include)
target_link_libraries(coinshift_core PUBLIC bt_core)

if(BT_WITH_NET)
  add_library(coinshift_node STATIC src/wallet.cpp src/app.cpp src/l1rpc.cpp)
  target_link_libraries(coinshift_node PUBLIC coinshift_core bt_net)
  add_executable(coinshift src/main.cpp)
  target_link_libraries(coinshift PRIVATE coinshift_node)
  add_executable(coinshift-cli src/cli.cpp)
  target_link_libraries(coinshift-cli PRIVATE coinshift_core bt_net)
  # Fetch full blocks from a public betanet peer over their QUIC protocol (client only).
  add_executable(coinshift-fetch-blocks tools/fetch_blocks.cpp)
  target_link_libraries(coinshift-fetch-blocks PRIVATE coinshift_core bt_net)
endif()

# Replay steps for the oracle, and our per-block state digests.
add_executable(coinshift-replay tools/replay.cpp)
target_link_libraries(coinshift-replay PRIVATE coinshift_core)
add_executable(coinshift-simcheck tools/simcheck_main.cpp)
target_link_libraries(coinshift-simcheck PRIVATE coinshift_core)
