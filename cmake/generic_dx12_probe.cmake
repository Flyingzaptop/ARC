option(ARC_GENERIC_DX12_PROBE "Build opt-in x64 read-only DX12 process probe" OFF)
if(WIN32 AND ARC_GENERIC_DX12_PROBE)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Generic DX12 probe currently supports x64 only")
    endif()
    enable_language(C)
    add_library(arc-generic-gpu-control STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_gpu_control.cpp)
    target_include_directories(arc-generic-gpu-control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12)
    target_compile_definitions(arc-generic-gpu-control PUBLIC NOMINMAX)
    target_link_libraries(arc-generic-gpu-control PUBLIC d3d12)
    add_library(arc-generic-shader-transform STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_shader_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_pcf_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_zero_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_edge_transform.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_uniform_access.cpp)
    target_include_directories(arc-generic-shader-transform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12)
    add_executable(arc-shader-tool ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/shader_tool.cpp)
    target_compile_definitions(arc-shader-tool PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(arc-shader-tool PRIVATE arc-generic-shader-transform)
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_gpu_profile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_command_mirror.cpp)
    target_compile_definitions(arc-dx12-probe PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(arc-dx12-probe PRIVATE arc::core arc-minhook arc-generic-binding arc-generic-gpu-control arc-generic-shader-transform d3d12 dxgi dxguid user32 d3dcompiler bcrypt)
    add_executable(arc-dx12-probe-launch ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/dx12_probe_launch.cpp)
    target_compile_definitions(arc-dx12-probe-launch PRIVATE UNICODE _UNICODE NOMINMAX)
    if(ARC_BUILD_TESTS)
        add_executable(arc-shader-transform-native-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/shader_transform_native.cpp)
        target_compile_definitions(arc-shader-transform-native-tests PRIVATE UNICODE _UNICODE NOMINMAX)
        target_link_libraries(arc-shader-transform-native-tests PRIVATE arc-generic-shader-transform arc-generic-gpu-control d3d12 dxgi)
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
            add_test(NAME arc-generic-probe-native-tests COMMAND arc-generic-probe-native-tests $<TARGET_FILE:arc-dx12-probe>)
            set_tests_properties(arc-generic-probe-native-tests PROPERTIES LABELS gpu TIMEOUT 45 RUN_SERIAL TRUE)
        endif()
    endif()
endif()
