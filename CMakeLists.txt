cmake_minimum_required(VERSION 3.24)

project(ARC VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ARC_BUILD_TESTS "Build ARC tests" ON)

add_library(arc-core STATIC
    src/core/clock.cpp
    src/core/event_ring.cpp
    src/core/resource_graph.cpp
    src/core/trace_writer.cpp
    src/core/trace_reader.cpp
)
add_library(arc::core ALIAS arc-core)

target_include_directories(arc-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(arc-core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(arc-core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(arc-core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(arc-trace src/tools/arc_trace_main.cpp)
target_link_libraries(arc-trace PRIVATE arc::core)

add_executable(arc-calibrate src/calibrate/arc_calibrate_main.cpp)
target_link_libraries(arc-calibrate PRIVATE arc::core)

if(WIN32)
    add_library(arc-dx12-observer STATIC
        src/backends/dx12/dx12_observer.cpp
    )
    target_include_directories(arc-dx12-observer PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(arc-dx12-observer PUBLIC arc::core d3d12 dxgi)
    target_compile_features(arc-dx12-observer PUBLIC cxx_std_23)
    target_link_libraries(arc-calibrate PRIVATE arc-dx12-observer dxgi)

    add_executable(dx12-memory-pressure samples/dx12_memory_pressure/main.cpp)
    target_link_libraries(dx12-memory-pressure PRIVATE arc-dx12-observer)
endif()

if(ARC_BUILD_TESTS)
    enable_testing()
    add_executable(arc-core-tests tests/core_tests.cpp)
    target_link_libraries(arc-core-tests PRIVATE arc::core)
    add_test(NAME arc-core-tests COMMAND arc-core-tests)
endif()
