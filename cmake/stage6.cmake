target_sources(arc-core PRIVATE
    src/core/adaptive_quality.cpp
    src/core/action_effect_tracker.cpp
    src/core/adaptive_quality_controller.cpp
)

if(ARC_BUILD_TESTS)
    add_executable(arc-adaptive-quality-tests tests/adaptive_quality_tests.cpp)
    target_link_libraries(arc-adaptive-quality-tests PRIVATE arc::core)
    add_test(NAME arc-adaptive-quality-tests COMMAND arc-adaptive-quality-tests)

    add_executable(arc-action-effect-tracker-tests tests/action_effect_tracker_tests.cpp)
    target_link_libraries(arc-action-effect-tracker-tests PRIVATE arc::core)
    add_test(NAME arc-action-effect-tracker-tests COMMAND arc-action-effect-tracker-tests)

    add_executable(arc-adaptive-quality-controller-tests tests/adaptive_quality_controller_tests.cpp)
    target_link_libraries(arc-adaptive-quality-controller-tests PRIVATE arc::core)
    add_test(NAME arc-adaptive-quality-controller-tests COMMAND arc-adaptive-quality-controller-tests)
endif()

if(WIN32)
    add_executable(dx12-adaptive-quality-benchmark samples/adaptive_quality_benchmark.cpp)
    target_link_libraries(dx12-adaptive-quality-benchmark PRIVATE arc::core d3d12 dxgi d3dcompiler)
    target_compile_features(dx12-adaptive-quality-benchmark PRIVATE cxx_std_23)
    # windows.h defines min/max macros unless NOMINMAX is set. The benchmark
    # intentionally uses std::min/std::max throughout policy and timing code.
    target_compile_definitions(dx12-adaptive-quality-benchmark PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    add_executable(arc-stage6-benchmark-ui WIN32 src/launcher/stage6_benchmark_ui.cpp)
    target_link_libraries(arc-stage6-benchmark-ui PRIVATE shell32)
    target_compile_features(arc-stage6-benchmark-ui PRIVATE cxx_std_23)
    target_compile_definitions(arc-stage6-benchmark-ui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    if(ARC_GPU_TESTS)
        add_test(NAME dx12-adaptive-quality-benchmark-smoke COMMAND dx12-adaptive-quality-benchmark --seconds 6 --headless --output traces/adaptive-quality-smoke.json)
        set_tests_properties(dx12-adaptive-quality-benchmark-smoke PROPERTIES LABELS gpu TIMEOUT 30 RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()
