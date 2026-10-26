cmake_minimum_required(VERSION 3.22)
project(backbone_coinshift VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The shared engine (git submodule ../backbone-core at core/): BT_* options, dependencies
# (core/depends/built/<host> when present), bt_core, bt_net, bt_test_harness,
# bt_core_test_cases. -ffp-contract=off is propagated by bt_core.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt")
  message(FATAL_ERROR "core/ is empty: run `git submodule update --init`")
endif()
add_subdirectory(core)

add_subdirectory(chains/coinshift)

# Our app of Coinshift (doc/app.md): coinshift-app, the node with our interface; and its pages as a pack for the unified
# wallet (target coinshift-app-pack). Needs WebKitGTK (Linux) or WebView2 (Windows), npm, and the sidechain template's
# repository (BT_KIT_DIR) at the commit of app/kit.commit.
option(BT_BUILD_APP "Build coinshift-app" OFF)
if(BT_BUILD_APP)
  include(core/cmake/BtDesktopApp.cmake)
  bt_desktop_app(coinshift-app STEM coinshift NAME Coinshift VERSION ${PROJECT_VERSION}
                 CHAIN ${CMAKE_SOURCE_DIR}/app/chain MANIFEST ${CMAKE_SOURCE_DIR}/app/manifest.json
                 KIT_COMMIT_FILE ${CMAKE_SOURCE_DIR}/app/kit.commit MAIN ${CMAKE_SOURCE_DIR}/chains/coinshift/app/main.cpp NODE_TARGET coinshift)
endif()

if(BT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
