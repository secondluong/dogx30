cmake_minimum_required(VERSION 3.16)
project(dogx30_rk3588 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(x30_core
  src/protocol.cpp
  src/udp_endpoint.cpp
  src/motion_client.cpp
  src/terrain_client.cpp
  src/gait_coordinator.cpp
  src/json.cpp
  src/gateway_config.cpp
  src/ws_server.cpp
  src/media_registry.cpp
  src/ptz_client.cpp
  src/net_util.cpp
  src/body_monitor.cpp
  src/xmlrpc.cpp
  src/ros_client.cpp
  src/point_cloud.cpp
  src/cloud_bridge.cpp
  src/localizer.cpp
  src/robot_service.cpp
)
target_include_directories(x30_core PUBLIC include)

# 遥测结构体依赖匿名 struct（GCC/Clang/MSVC 均支持的扩展），因此不开 -pedantic。
if(MSVC)
  target_compile_options(x30_core PRIVATE /W3 /utf-8)
else()
  target_compile_options(x30_core PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(x30_core PUBLIC Threads::Threads)

if(WIN32)
  target_link_libraries(x30_core PUBLIC ws2_32)
endif()

add_executable(x30_gateway src/main.cpp)
target_link_libraries(x30_gateway PRIVATE x30_core)

add_executable(x30_localizer_test src/localizer_test.cpp)
target_link_libraries(x30_localizer_test PRIVATE x30_core)
if(MSVC)
  target_compile_options(x30_gateway PRIVATE /utf-8)
endif()
