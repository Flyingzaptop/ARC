if(WIN32)
    target_sources(arc-dx12-observer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/host_adapter.cpp)

    if(ARC_BUILD_TESTS)
        add_executable(arc-dx12-host-adapter-tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/dx12_host_adapter_tests.cpp)
        target_link_libraries(arc-dx12-host-adapter-tests PRIVATE arc-dx12-observer)
        target_compile_features(arc-dx12-host-adapter-tests PRIVATE cxx_std_23)
        target_compile_definitions(arc-dx12-host-adapter-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME arc-dx12-host-adapter-tests COMMAND arc-dx12-host-adapter-tests)
        set_tests_properties(arc-dx12-host-adapter-tests PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
endif()
