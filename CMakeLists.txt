cmake_minimum_required(VERSION 3.20)
project(can_telemetry_replay VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
find_package(Boost REQUIRED)

add_library(can_telemetry_core STATIC dbc/dbc_parser.cpp v2c/v2c_transcoder.cpp)
target_compile_features(can_telemetry_core PUBLIC cxx_std_20)
target_include_directories(can_telemetry_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(can_telemetry_core PUBLIC Boost::headers)

add_executable(can-telemetry-replay app/replay.cpp)
target_link_libraries(can-telemetry-replay PRIVATE can_telemetry_core)

option(BUILD_EXAMPLES "Build the live transcoder demonstration" OFF)
if(BUILD_EXAMPLES)
  add_executable(can-transcoder-example example/example.cpp)
  target_link_libraries(can-transcoder-example PRIVATE can_telemetry_core)
endif()

install(TARGETS can-telemetry-replay)
install(FILES LICENSE NOTICE LICENSES/Linux-CAN.txt DESTINATION "${CMAKE_INSTALL_DOCDIR}")

if(BUILD_TESTING)
  add_executable(v2c-clock-test tests/v2c_clock.cpp)
  target_link_libraries(v2c-clock-test PRIVATE can_telemetry_core)
  add_test(NAME v2c-clock COMMAND v2c-clock-test)
  add_test(NAME v2c-no-group COMMAND v2c-clock-test no-group)
  add_test(NAME v2c-zero-group COMMAND v2c-clock-test zero-group)
  set_tests_properties(v2c-clock v2c-no-group v2c-zero-group PROPERTIES TIMEOUT 3)
  add_test(NAME offline-replay
    COMMAND "${CMAKE_COMMAND}"
      "-DREPLAY=$<TARGET_FILE:can-telemetry-replay>"
      "-DEXAMPLES=${CMAKE_CURRENT_SOURCE_DIR}/example"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/replay.cmake")
  add_test(NAME aggregate-replay
    COMMAND "${CMAKE_COMMAND}"
      "-DREPLAY=$<TARGET_FILE:can-telemetry-replay>"
      "-DEXAMPLES=${CMAKE_CURRENT_SOURCE_DIR}/example"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/aggregate.cmake")
endif()
