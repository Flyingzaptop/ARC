if(ARC_BUILD_TESTS)
    add_executable(arc-closed-loop-quality-sim-tests tests/closed_loop_quality_sim_tests.cpp)
    target_link_libraries(arc-closed-loop-quality-sim-tests PRIVATE arc::core)
    add_test(NAME arc-closed-loop-quality-sim-tests COMMAND arc-closed-loop-quality-sim-tests)

    add_executable(arc-quality-restore-memory-pressure-tests tests/quality_restore_memory_pressure_tests.cpp)
    target_link_libraries(arc-quality-restore-memory-pressure-tests PRIVATE arc::core)
    add_test(NAME arc-quality-restore-memory-pressure-tests COMMAND arc-quality-restore-memory-pressure-tests)
endif()

if(WIN32)
    add_executable(dx12-closed-loop-mixed-graphics-benchmark samples/closed_loop_mixed_graphics_benchmark.cpp)
    target_link_libraries(dx12-closed-loop-mixed-graphics-benchmark PRIVATE arc::core d3d12 dxgi d3dcompiler)
    target_compile_features(dx12-closed-loop-mixed-graphics-benchmark PRIVATE cxx_std_23)
    target_compile_definitions(dx12-closed-loop-mixed-graphics-benchmark PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    add_executable(arc-stage8-benchmark-ui WIN32 src/launcher/stage8_benchmark_ui.cpp)
    target_link_libraries(arc-stage8-benchmark-ui PRIVATE shell32)
    target_compile_features(arc-stage8-benchmark-ui PRIVATE cxx_std_23)
    target_compile_definitions(arc-stage8-benchmark-ui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    if(ARC_GPU_TESTS)
        add_test(
            NAME dx12-closed-loop-mixed-graphics-benchmark-smoke
            COMMAND dx12-closed-loop-mixed-graphics-benchmark
                --seconds 12
                --probe-frames 4
                --control-frames 8
                --output traces/closed-loop-mixed-graphics-smoke.json)
        set_tests_properties(
            dx12-closed-loop-mixed-graphics-benchmark-smoke
            PROPERTIES
                LABELS gpu
                TIMEOUT 90
                RUN_SERIAL TRUE
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()
