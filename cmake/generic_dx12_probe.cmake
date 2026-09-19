option(ARC_GENERIC_DX12_PROBE "Build opt-in x64 read-only DX12 process probe" OFF)
if(WIN32 AND ARC_GENERIC_DX12_PROBE)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Generic DX12 probe currently supports x64 only")
    endif()
    enable_language(C)
    add_library(arc-minhook STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/buffer.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/hook.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/trampoline.c
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/src/hde/hde64.c)
    target_include_directories(arc-minhook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/include)
    add_library(arc-dx12-probe SHARED ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/dx12/generic_probe.cpp)
    target_compile_definitions(arc-dx12-probe PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(arc-dx12-probe PRIVATE arc::core arc-minhook d3d12 dxgi dxguid user32)
    add_executable(arc-dx12-probe-launch ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/dx12_probe_launch.cpp)
    target_compile_definitions(arc-dx12-probe-launch PRIVATE UNICODE _UNICODE NOMINMAX)
endif()
