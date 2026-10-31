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
foreach(profile_test IN ITEMS controller_events absolute_dimmer local_dimmer mesh_capture controller_discovery controller_tables controller_bindings)
    add_executable(test_${profile_test} test_${profile_test}.cpp ${COMPONENT_DIR}/controller_bindings.cpp ${COMPONENT_DIR}/mqtt_discovery.cpp stubs/stubs.cpp)
    target_include_directories(test_${profile_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${COMPONENT_DIR})
    target_compile_options(test_${profile_test} PRIVATE -UNDEBUG)
    target_link_libraries(test_${profile_test} PRIVATE ${ARDUINOJSON_TARGET})
    add_test(NAME ${profile_test} COMMAND test_${profile_test})
endforeach()
