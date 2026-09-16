cmake_minimum_required(VERSION 3.24)

project(ARC VERSION 0.1.0 LANGUAGES CXX)

set(ARC_MSVC_INCLUDE_PREFIX "" CACHE STRING "Override localized MSVC include diagnostic prefix")
if(MSVC AND ARC_MSVC_INCLUDE_PREFIX)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${ARC_MSVC_INCLUDE_PREFIX}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ARC_BUILD_TESTS "Build ARC tests" ON)
option(ARC_GPU_TESTS "Run GPU-backed synthetic tests on this machine" OFF)
option(ARC_STRESS_TESTS "Build and register long-running adversarial tests" OFF)

add_library(arc-core STATIC
    src/core/clock.cpp
    src/core/session.cpp
    src/core/multi_session.cpp
    src/core/event_ring.cpp
    src/core/resource_graph.cpp
    src/core/residency.cpp
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
add_executable(arc-trace-viewer src/tools/trace_viewer.cpp)
target_link_libraries(arc-trace-viewer PRIVATE arc::core)

if(WIN32)
    add_library(arc-dx12-observer STATIC
        src/backends/dx12/dx12_observer.cpp
        src/backends/dx12/views.cpp
        src/backends/dx12/barriers.cpp
        src/backends/dx12/residency.cpp
    )
    target_include_directories(arc-dx12-observer PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(arc-dx12-observer PUBLIC arc::core d3d12 dxgi)
    target_compile_features(arc-dx12-observer PUBLIC cxx_std_23)

    add_executable(arc-calibrate src/calibrate/arc_calibrate_main.cpp)
    target_link_libraries(arc-calibrate PRIVATE arc::core arc-dx12-observer dxgi cabinet)

    foreach(sample dx12-memory-pressure dx12-texture-pressure dx12-mixed-resources)
        add_executable(${sample} samples/observer_validation.cpp)
        target_compile_definitions(${sample} PRIVATE ARC_SAMPLE_NAME="${sample}")
        target_link_libraries(${sample} PRIVATE arc-dx12-observer d3dcompiler psapi)
    endforeach()
    add_executable(dx12-residency-lab samples/residency_lab.cpp)
    target_link_libraries(dx12-residency-lab PRIVATE arc-dx12-observer)
endif()

if(ARC_BUILD_TESTS)
    enable_testing()
    add_executable(arc-core-tests tests/core_tests.cpp)
    target_link_libraries(arc-core-tests PRIVATE arc::core)
    add_test(NAME arc-core-tests COMMAND arc-core-tests)

    add_executable(arc-residency-policy-tests tests/residency_policy_tests.cpp)
    target_link_libraries(arc-residency-policy-tests PRIVATE arc::core)
    add_test(NAME arc-residency-policy-tests COMMAND arc-residency-policy-tests)

    add_executable(arc-residency-autonomous-sim-tests tests/residency_autonomous_sim_tests.cpp)
    target_link_libraries(arc-residency-autonomous-sim-tests PRIVATE arc::core)
    add_test(NAME arc-residency-autonomous-sim-tests COMMAND arc-residency-autonomous-sim-tests)

    if(ARC_STRESS_TESTS)
        add_executable(arc-multisession-stress tests/multisession_stress.cpp)
        target_link_libraries(arc-multisession-stress PRIVATE arc::core)
        add_test(NAME arc-multisession-stress COMMAND arc-multisession-stress 3 2000000)
        set_tests_properties(arc-multisession-stress PROPERTIES LABELS stress TIMEOUT 300 RUN_SERIAL TRUE)
    endif()
    if(WIN32)
        add_executable(arc-dx12-view-tests tests/dx12_view_tests.cpp)
        target_link_libraries(arc-dx12-view-tests PRIVATE arc-dx12-observer)
        add_test(NAME arc-dx12-view-tests COMMAND arc-dx12-view-tests)
        if(ARC_GPU_TESTS)
            add_executable(arc-dx12-enhanced-tests tests/dx12_enhanced_tests.cpp)
            target_link_libraries(arc-dx12-enhanced-tests PRIVATE arc-dx12-observer)
            add_test(NAME arc-dx12-enhanced-tests COMMAND arc-dx12-enhanced-tests)
            set_tests_properties(arc-dx12-enhanced-tests PROPERTIES LABELS gpu TIMEOUT 60 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
            foreach(sample dx12-memory-pressure dx12-texture-pressure dx12-mixed-resources)
                add_test(NAME ${sample} COMMAND ${sample} full 30)
                set_tests_properties(${sample} PROPERTIES LABELS gpu TIMEOUT 60 RUN_SERIAL TRUE)
            endforeach()
            add_test(NAME dx12-residency-lab-baseline COMMAND dx12-residency-lab baseline 24 2)
            add_test(NAME dx12-residency-lab-arc COMMAND dx12-residency-lab arc 24 2)
            add_test(NAME dx12-residency-lab-auto COMMAND dx12-residency-lab auto 24 2)
            set_tests_properties(dx12-residency-lab-baseline dx12-residency-lab-arc dx12-residency-lab-auto PROPERTIES LABELS gpu TIMEOUT 90 RUN_SERIAL TRUE)
        endif()
    endif()
endif()
