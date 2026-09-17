target_sources(arc-core PRIVATE
    src/core/render_quality_model.cpp
    src/core/render_candidate_catalog.cpp
)

if(ARC_BUILD_TESTS)
    add_executable(arc-render-quality-model-tests tests/render_quality_model_tests.cpp)
    target_link_libraries(arc-render-quality-model-tests PRIVATE arc::core)
    add_test(NAME arc-render-quality-model-tests COMMAND arc-render-quality-model-tests)

    add_executable(arc-render-candidate-catalog-tests tests/render_candidate_catalog_tests.cpp)
    target_link_libraries(arc-render-candidate-catalog-tests PRIVATE arc::core)
    add_test(NAME arc-render-candidate-catalog-tests COMMAND arc-render-candidate-catalog-tests)
endif()

if(WIN32)
    add_executable(dx12-mixed-graphics-benchmark samples/mixed_graphics_benchmark.cpp)
    target_link_libraries(dx12-mixed-graphics-benchmark PRIVATE arc::core d3d12 dxgi d3dcompiler)
    target_compile_features(dx12-mixed-graphics-benchmark PRIVATE cxx_std_23)
    target_compile_definitions(dx12-mixed-graphics-benchmark PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    add_executable(arc-stage7-benchmark-ui WIN32 src/launcher/stage7_benchmark_ui.cpp)
    target_link_libraries(arc-stage7-benchmark-ui PRIVATE shell32)
    target_compile_features(arc-stage7-benchmark-ui PRIVATE cxx_std_23)
    target_compile_definitions(arc-stage7-benchmark-ui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    if(ARC_GPU_TESTS)
        add_test(NAME dx12-mixed-graphics-benchmark-smoke
                 COMMAND dx12-mixed-graphics-benchmark --seconds 6 --output traces/mixed-graphics-smoke.json)
        set_tests_properties(dx12-mixed-graphics-benchmark-smoke PROPERTIES
            LABELS gpu TIMEOUT 30 RUN_SERIAL TRUE WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()
