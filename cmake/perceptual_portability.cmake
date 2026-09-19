set(ARC_MICROSOFT_PERCEPTUAL_SAMPLE "" CACHE PATH "Patched pinned D3D12HelloTexture directory")
set(ARC_AGILITY_NATIVE "" CACHE PATH "Existing Microsoft.Direct3D.D3D12 1.618.3 build/native directory")
set(ARC_DXC_EXECUTABLE "" CACHE FILEPATH "Pinned DirectX shader compiler executable")
if(WIN32 AND ARC_MICROSOFT_PERCEPTUAL_SAMPLE AND ARC_AGILITY_NATIVE)
    if(NOT EXISTS "${ARC_DXC_EXECUTABLE}")
        message(FATAL_ERROR "ARC_DXC_EXECUTABLE is required to compile the upstream shader assets")
    endif()
    set(sample "${ARC_MICROSOFT_PERCEPTUAL_SAMPLE}")
    add_executable(arc-microsoft-perceptual WIN32
        ${sample}/Main.cpp ${sample}/D3D12HelloTexture.cpp
        ${sample}/DXSample.cpp ${sample}/Win32Application.cpp)
    target_include_directories(arc-microsoft-perceptual PRIVATE ${sample} ${ARC_AGILITY_NATIVE} ${ARC_AGILITY_NATIVE}/include)
    target_compile_definitions(arc-microsoft-perceptual PRIVATE UNICODE _UNICODE NOMINMAX)
    target_compile_options(arc-microsoft-perceptual PRIVATE /permissive)
    target_link_libraries(arc-microsoft-perceptual PRIVATE arc::core d3d12 dxgi d3dcompiler dxguid)
    add_custom_command(TARGET arc-microsoft-perceptual POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:arc-microsoft-perceptual>/D3D12
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${sample}/shaders.hlsl $<TARGET_FILE_DIR:arc-microsoft-perceptual>/shaders.hlsl
        COMMAND ${ARC_DXC_EXECUTABLE} -T vs_6_0 -E VSMain -Fo $<TARGET_FILE_DIR:arc-microsoft-perceptual>/shaders_VSMain.cso ${sample}/shaders.hlsl
        COMMAND ${ARC_DXC_EXECUTABLE} -T ps_6_0 -E PSMain -Fo $<TARGET_FILE_DIR:arc-microsoft-perceptual>/shaders_PSMain.cso ${sample}/shaders.hlsl
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ARC_AGILITY_NATIVE}/bin/x64/D3D12Core.dll $<TARGET_FILE_DIR:arc-microsoft-perceptual>/D3D12/D3D12Core.dll)
endif()
