if(WIN32)
    target_sources(arc-dx12-observer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/host_adapter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/host_adapter_extra.cpp)
    # ID3D12Fence::GetCompletedValue returns UINT64_MAX on device removal, but
    # the Windows SDK does not provide a D3D12_FENCE_VALUE_MAX symbol.
    target_compile_definitions(arc-dx12-observer PRIVATE D3D12_FENCE_VALUE_MAX=UINT64_MAX)

    add_executable(dx12-mega-stage-b-host-renderer
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/mega_stage_b_host_renderer.cpp)
    target_link_libraries(dx12-mega-stage-b-host-renderer PRIVATE arc-dx12-observer d3dcompiler user32)
    target_compile_features(dx12-mega-stage-b-host-renderer PRIVATE cxx_std_23)
    target_compile_definitions(dx12-mega-stage-b-host-renderer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

    if(ARC_BUILD_TESTS)
        add_executable(arc-dx12-host-adapter-tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/dx12_host_adapter_tests.cpp)
        target_link_libraries(arc-dx12-host-adapter-tests PRIVATE arc-dx12-observer)
        target_compile_features(arc-dx12-host-adapter-tests PRIVATE cxx_std_23)
        target_compile_definitions(arc-dx12-host-adapter-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME arc-dx12-host-adapter-tests COMMAND arc-dx12-host-adapter-tests)
        set_tests_properties(arc-dx12-host-adapter-tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

        add_executable(arc-dx12-host-adapter-surface-tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/dx12_host_adapter_surface_tests.cpp)
        target_link_libraries(arc-dx12-host-adapter-surface-tests PRIVATE arc-dx12-observer)
        target_compile_features(arc-dx12-host-adapter-surface-tests PRIVATE cxx_std_23)
        target_compile_definitions(arc-dx12-host-adapter-surface-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME arc-dx12-host-adapter-surface-tests COMMAND arc-dx12-host-adapter-surface-tests)
        set_tests_properties(arc-dx12-host-adapter-surface-tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        # The one-click bootstrap explicitly builds the main host-adapter test
        # target; make the modern-surface regression part of that build graph.
        add_dependencies(arc-dx12-host-adapter-tests arc-dx12-host-adapter-surface-tests)

        if(ARC_GPU_TESTS)
            add_test(
                NAME dx12-mega-stage-b-host-renderer-smoke
                COMMAND dx12-mega-stage-b-host-renderer
                    --seconds 30
                    --probe-frames 4
                    --control-frames 8
                    --output traces/mega-stage-b-smoke.json)
            set_tests_properties(
                dx12-mega-stage-b-host-renderer-smoke
                PROPERTIES
                    LABELS gpu
                    TIMEOUT 180
                    SKIP_RETURN_CODE 77
                    RUN_SERIAL TRUE
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        endif()
    endif()
endif()
