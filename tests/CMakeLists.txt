cmake_minimum_required(VERSION 3.14)
project(avionmesh_tests CXX)

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
set(AVIONMESH_LIB /home/oyvind/avionmesh-cpp)
set(ARDUINOJSON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../.esphome/build/avionmesh/managed_components/bblanchon__arduinojson/src)

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
)

add_executable(avionmesh_tests ${SOURCES})

target_include_directories(avionmesh_tests PRIVATE
    # stubs/ shadows esphome/* and recsrmesh/* headers
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${COMPONENT_DIR}
    ${AVIONMESH_LIB}/include
    ${ARDUINOJSON_DIR}
)

target_compile_definitions(avionmesh_tests PRIVATE
    ARDUINOJSON_ENABLE_STD_STRING=1
    ARDUINOJSON_USE_LONG_LONG=1
)

target_link_libraries(avionmesh_tests PRIVATE GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(avionmesh_tests)
