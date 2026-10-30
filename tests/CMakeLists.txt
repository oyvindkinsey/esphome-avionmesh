cmake_minimum_required(VERSION 3.14)
project(avionmesh_tests CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- GoogleTest via FetchContent ----
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---- Sources ----
set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../components/avionmesh)
# avionmesh-cpp: sibling checkout by default; override with -DAVIONMESH_LIB=...
set(AVIONMESH_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../../avionmesh-cpp"
    CACHE PATH "Path to avionmesh-cpp checkout")
if(NOT EXISTS ${AVIONMESH_LIB}/include)
    message(FATAL_ERROR
        "avionmesh-cpp not found at ${AVIONMESH_LIB}. Clone "
        "https://github.com/oyvindkinsey/avionmesh-cpp next to this repo, "
        "or pass -DAVIONMESH_LIB=<path>.")
endif()
# ArduinoJson: local ESPHome build tree if present, else FetchContent
set(ARDUINOJSON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../.esphome/build/avionmesh/managed_components/bblanchon__arduinojson/src
    CACHE PATH "ArduinoJson src directory")
if(EXISTS ${ARDUINOJSON_DIR}/ArduinoJson.h)
    add_library(arduinojson INTERFACE)
    target_include_directories(arduinojson INTERFACE ${ARDUINOJSON_DIR})
    set(ARDUINOJSON_TARGET arduinojson)
else()
    set(ARDUINOJSON_TARGET ArduinoJson)
    FetchContent_Declare(arduinojson
        GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
        GIT_TAG        v7.4.2)
    FetchContent_MakeAvailable(arduinojson)
endif()

set(SOURCES
    ${COMPONENT_DIR}/avionmesh_hub.cpp
    ${COMPONENT_DIR}/mqtt_discovery.cpp
    ${COMPONENT_DIR}/device_db.cpp
    ${COMPONENT_DIR}/controller_bindings.cpp
    ${COMPONENT_DIR}/controller_binding_refresh.cpp
    ${COMPONENT_DIR}/keypad_setup.cpp
    ${COMPONENT_DIR}/keypad_setup_journal.cpp
    ${AVIONMESH_LIB}/src/avionmesh.cpp
    ${AVIONMESH_LIB}/src/parse.cpp
    ${AVIONMESH_LIB}/src/commands.cpp
    ${AVIONMESH_LIB}/src/types.cpp
    stubs/stubs.cpp
    test_group_latch.cpp
    test_group_latch_v2.cpp
    test_mqtt_commands.cpp
    test_sse_events.cpp
    test_api_control.cpp
    test_discover_ping.cpp
    test_uuid_identity.cpp
    test_db_batch.cpp
    test_upstream_integration.cpp
    test_controller_readiness.cpp
    test_controller_memory.cpp
    test_battery_keypad_lifecycle.cpp
)

add_executable(avionmesh_tests ${SOURCES})

target_include_directories(avionmesh_tests PRIVATE
    # stubs/ shadows esphome/* and recsrmesh/* headers
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${COMPONENT_DIR}
    ${AVIONMESH_LIB}/include
)

target_compile_definitions(avionmesh_tests PRIVATE
    ARDUINOJSON_ENABLE_STD_STRING=1
    ARDUINOJSON_USE_LONG_LONG=1
)

target_link_libraries(avionmesh_tests PRIVATE GTest::gtest_main ${ARDUINOJSON_TARGET})

include(GoogleTest)
gtest_discover_tests(avionmesh_tests)

# Protocol fixtures use standalone assertion executables; keep assertions enabled.
foreach(profile_test IN ITEMS controller_profile controller_events absolute_dimmer local_dimmer mesh_capture controller_discovery controller_tables controller_bindings keypad_setup keypad_setup_journal)
    add_executable(test_${profile_test} test_${profile_test}.cpp ${COMPONENT_DIR}/controller_bindings.cpp ${COMPONENT_DIR}/keypad_setup_journal.cpp ${COMPONENT_DIR}/mqtt_discovery.cpp stubs/stubs.cpp)
    target_include_directories(test_${profile_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${COMPONENT_DIR})
    target_compile_options(test_${profile_test} PRIVATE -UNDEBUG)
    target_link_libraries(test_${profile_test} PRIVATE ${ARDUINOJSON_TARGET})
    add_test(NAME ${profile_test} COMMAND test_${profile_test})
endforeach()

# Exercise both released-library fallback and additive observer API glue.
find_package(Threads REQUIRED)
foreach(rx_mode IN ITEMS legacy observer)
    add_executable(test_controller_rx_${rx_mode} test_controller_rx.cpp)
    target_include_directories(test_controller_rx_${rx_mode} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${COMPONENT_DIR})
    target_compile_options(test_controller_rx_${rx_mode} PRIVATE -UNDEBUG)
    if(rx_mode STREQUAL "observer")
        target_compile_definitions(test_controller_rx_${rx_mode} PRIVATE AVIONMESH_TEST_RX_OBSERVER=1)
    endif()
    target_link_libraries(test_controller_rx_${rx_mode} PRIVATE Threads::Threads)
    add_test(NAME controller_rx_${rx_mode} COMMAND test_controller_rx_${rx_mode})
endforeach()

# Exercise ESP-IDF sampling through read-only API stubs, including disabled task metrics.
foreach(stack_metrics IN ITEMS enabled disabled)
    add_executable(test_controller_memory_${stack_metrics} test_controller_memory_platform.cpp)
    target_include_directories(test_controller_memory_${stack_metrics} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/memory_platform_stubs ${COMPONENT_DIR})
    target_compile_definitions(test_controller_memory_${stack_metrics} PRIVATE USE_ESP32)
    if(stack_metrics STREQUAL "disabled")
        target_compile_definitions(test_controller_memory_${stack_metrics} PRIVATE INCLUDE_uxTaskGetStackHighWaterMark=0)
    endif()
    target_compile_options(test_controller_memory_${stack_metrics} PRIVATE -UNDEBUG)
    target_link_libraries(test_controller_memory_${stack_metrics} PRIVATE ${ARDUINOJSON_TARGET})
    add_test(NAME controller_memory_${stack_metrics} COMMAND test_controller_memory_${stack_metrics})
endforeach()
# Actual production SSE queue, shared-context lifetime and paced serializer.
add_executable(test_sse_sessions test_sse_sessions.cpp ${AVIONMESH_LIB}/src/types.cpp)
target_include_directories(test_sse_sessions PRIVATE ${COMPONENT_DIR} ${AVIONMESH_LIB}/include)
target_compile_options(test_sse_sessions PRIVATE -UNDEBUG)
target_link_libraries(test_sse_sessions PRIVATE ${ARDUINOJSON_TARGET} Threads::Threads)
add_test(NAME sse_sessions COMMAND test_sse_sessions)

