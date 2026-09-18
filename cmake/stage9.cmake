target_sources(arc-core PRIVATE
    src/core/quality_admission.cpp
    src/core/global_action_arbiter.cpp
    src/core/unified_runtime_governor.cpp
)

if(ARC_BUILD_TESTS)
    add_executable(arc-quality-admission-tests tests/quality_admission_tests.cpp)
    target_link_libraries(arc-quality-admission-tests PRIVATE arc::core)
    add_test(NAME arc-quality-admission-tests COMMAND arc-quality-admission-tests)

    add_executable(arc-global-action-arbiter-tests tests/global_action_arbiter_tests.cpp)
    target_link_libraries(arc-global-action-arbiter-tests PRIVATE arc::core)
    add_test(NAME arc-global-action-arbiter-tests COMMAND arc-global-action-arbiter-tests)

    add_executable(arc-unified-runtime-governor-tests tests/unified_runtime_governor_tests.cpp)
    target_link_libraries(arc-unified-runtime-governor-tests PRIVATE arc::core)
    add_test(NAME arc-unified-runtime-governor-tests COMMAND arc-unified-runtime-governor-tests)
endif()

if(WIN32)
    add_executable(dx12-mega-stage-a-benchmark samples/mega_stage_a_benchmark.cpp)
    target_link_libraries(dx12-mega-stage-a-benchmark PRIVATE arc::core arc-dx12-observer d3dcompiler)
    target_compile_features(dx12-mega-stage-a-benchmark PRIVATE cxx_std_23)
    target_compile_definitions(dx12-mega-stage-a-benchmark PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    add_executable(arc-mega-stage-a-benchmark-ui WIN32 src/launcher/mega_stage_a_benchmark_ui.cpp)
    target_link_libraries(arc-mega-stage-a-benchmark-ui PRIVATE shell32)
    target_compile_features(arc-mega-stage-a-benchmark-ui PRIVATE cxx_std_23)
    target_compile_definitions(arc-mega-stage-a-benchmark-ui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    if(ARC_GPU_TESTS)
        add_test(
            NAME dx12-mega-stage-a-benchmark-smoke
            COMMAND dx12-mega-stage-a-benchmark
                --seconds 30
                --probe-frames 4
                --control-frames 8
                --output traces/mega-stage-a-smoke.json)
        set_tests_properties(
            dx12-mega-stage-a-benchmark-smoke
            PROPERTIES
                LABELS gpu
                TIMEOUT 120
                RUN_SERIAL TRUE
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mega_stage_b.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mega_stage_c.cmake)
