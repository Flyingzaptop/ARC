option(ARC_GENERIC_DX12_PROBE "Build opt-in x64 read-only DX12 process probe" OFF)
if(WIN32 AND ARC_GENERIC_DX12_PROBE)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Generic DX12 probe currently supports x64 only")
    endif()
    enable_language(C)
    add_library(arc-generic-worker-placement STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_worker_placement.cpp)
    target_include_directories(arc-generic-worker-placement PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12 ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(arc-generic-worker-placement PUBLIC NOMINMAX)
    add_library(arc-generic-gpu-control STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_gpu_control.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_spatial_probe.cpp)
    target_include_directories(arc-generic-gpu-control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12 ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(arc-generic-gpu-control PUBLIC NOMINMAX)
    target_link_libraries(arc-generic-gpu-control PUBLIC d3d12 d3dcompiler)
    add_library(arc-generic-shader-transform STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_shader_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_probe_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_pcf_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_zero_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_edge_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_spatial_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_mip_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_sample_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_uniform_access.cpp)
    target_include_directories(arc-generic-shader-transform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12)
    add_executable(arc-shader-tool ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/shader_tool.cpp)
    target_compile_definitions(arc-shader-tool PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(arc-shader-tool PRIVATE arc-generic-shader-transform ole32)
    add_library(arc-generic-binding STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_binding_state.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_binding_admission.cpp)
    target_include_directories(arc-generic-binding PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12)
    target_compile_definitions(arc-generic-binding PUBLIC NOMINMAX)
    target_link_libraries(arc-generic-binding PUBLIC d3d12 arc::core)
    add_library(arc-minhook STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/buffer.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/hook.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/trampoline.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/hde/hde64.c)
    target_include_directories(arc-minhook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include)
    add_library(arc-dx12-probe SHARED ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_probe.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_runtime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_readback.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_optimizer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_shader_cache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_cpu_workers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_auto_session.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_child_launch.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_gpu_profile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_command_mirror.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_pixel_optimizer.cpp)
    target_compile_definitions(arc-dx12-probe PRIVATE UNICODE _UNICODE NOMINMAX)
    target_include_directories(arc-dx12-probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
    target_link_libraries(arc-dx12-probe PRIVATE arc::core arc-minhook arc-generic-binding arc-generic-gpu-control arc-generic-shader-transform arc-generic-worker-placement d3d12 dxgi dxguid user32 d3dcompiler bcrypt psapi)
    add_executable(arc-dx12-probe-launch ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/dx12_probe_launch.cpp)
    target_link_libraries(arc-dx12-probe-launch PRIVATE psapi)
    target_compile_definitions(arc-dx12-probe-launch PRIVATE UNICODE _UNICODE NOMINMAX)
    if(ARC_BUILD_TESTS)
        add_executable(arc-quality-worker-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/quality_worker_native.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_cpu_workers.cpp)
        target_include_directories(arc-quality-worker-native-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12 ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
        target_compile_definitions(arc-quality-worker-native-tests PRIVATE NOMINMAX)
        target_link_libraries(arc-quality-worker-native-tests PRIVATE psapi arc-generic-worker-placement)
        target_compile_options(arc-quality-worker-native-tests PRIVATE /UNDEBUG)
        add_executable(arc-shader-cache-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_cache_tests.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_shader_cache.cpp)
        target_include_directories(arc-shader-cache-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12 ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
        target_compile_definitions(arc-shader-cache-tests PRIVATE NOMINMAX)
        target_link_libraries(arc-shader-cache-tests PRIVATE bcrypt arc-core)
        add_test(NAME arc-shader-cache-tests COMMAND arc-shader-cache-tests)
        add_executable(arc-gpu-execution-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu_execution_native.cpp)
        add_executable(arc-sparse-probe-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/sparse_probe_native.cpp)
        target_compile_definitions(arc-sparse-probe-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-sparse-probe-native-tests PRIVATE arc-generic-binding arc-generic-shader-transform arc-generic-gpu-control d3d12 dxgi d3dcompiler)
        target_compile_definitions(arc-gpu-execution-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-gpu-execution-native-tests PRIVATE arc-generic-binding arc-generic-shader-transform arc-generic-gpu-control d3d12 dxgi d3dcompiler)
        add_executable(arc-connection-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/connection_native.cpp)
        target_include_directories(arc-connection-native-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
        target_compile_definitions(arc-connection-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-connection-native-tests PRIVATE d3d12 dxgi d3dcompiler user32)
        add_executable(arc-worker-placement-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_placement_native.cpp)
        target_include_directories(arc-worker-placement-native-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json)
        target_link_libraries(arc-worker-placement-native-tests PRIVATE arc-generic-worker-placement)
        add_test(NAME arc-worker-placement-native-tests COMMAND arc-worker-placement-native-tests)
        set_tests_properties(arc-worker-placement-native-tests PROPERTIES TIMEOUT 20)
        add_executable(arc-generic-gpu-control-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_gpu_control_native.cpp)
        target_link_libraries(arc-generic-gpu-control-native-tests PRIVATE arc-generic-gpu-control)
        add_executable(arc-pixel-mip-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/pixel_mip_native.cpp)
        target_include_directories(arc-pixel-mip-native-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_definitions(arc-pixel-mip-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-pixel-mip-native-tests PRIVATE arc-generic-shader-transform arc-generic-gpu-control d3d12 dxgi d3dcompiler)
        add_executable(arc-shader-sm66-gpu-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_sm66_gpu_native.cpp)
        target_compile_definitions(arc-shader-sm66-gpu-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-shader-sm66-gpu-native-tests PRIVATE arc-generic-shader-transform d3d12 dxgi d3dcompiler)
        add_executable(arc-shader-sm66-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_sm66_native.cpp)
        target_link_libraries(arc-shader-sm66-native-tests PRIVATE arc-generic-shader-transform)
        target_compile_definitions(arc-shader-sm66-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        add_executable(arc-shader-transform-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_transform_native.cpp)
        target_compile_definitions(arc-shader-transform-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-shader-transform-native-tests PRIVATE arc-generic-shader-transform arc-generic-gpu-control d3d12 dxgi d3dcompiler)
        add_executable(arc-generic-binding-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_binding_state_tests.cpp)
        target_link_libraries(arc-generic-binding-tests PRIVATE arc-generic-binding)
        add_test(NAME arc-generic-binding-tests COMMAND arc-generic-binding-tests)
        add_executable(arc-generic-gpu-profile-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_gpu_profile_native.cpp)
        target_compile_definitions(arc-generic-gpu-profile-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-generic-gpu-profile-native-tests PRIVATE d3d12 dxgi d3dcompiler user32)
        add_executable(arc-generic-vrs-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_vrs_native.cpp)
        target_compile_definitions(arc-generic-vrs-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-generic-vrs-native-tests PRIVATE d3d12 dxgi d3dcompiler user32)
        add_executable(arc-generic-runtime-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_runtime_native.cpp)
        target_compile_definitions(arc-generic-runtime-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-generic-runtime-native-tests PRIVATE d3d12 dxgi user32)
        add_executable(arc-generic-probe-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/generic_probe_native.cpp)
        target_compile_definitions(arc-generic-probe-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-generic-probe-native-tests PRIVATE d3d12 dxgi user32)
        if(ARC_GPU_TESTS)
            add_test(NAME arc-generic-gpu-control-native-tests COMMAND arc-generic-gpu-control-native-tests)
            set_tests_properties(arc-generic-gpu-control-native-tests PROPERTIES LABELS gpu TIMEOUT 45 RUN_SERIAL TRUE)
            add_test(NAME arc-generic-probe-native-tests COMMAND arc-generic-probe-native-tests $<TARGET_FILE:arc-dx12-probe>)
            set_tests_properties(arc-generic-probe-native-tests PROPERTIES LABELS gpu TIMEOUT 45 RUN_SERIAL TRUE)
        endif()
    endif()
endif()
