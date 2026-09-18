target_sources(arc-core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/temporal_visibility.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/visual_importance.cpp
)

if(ARC_BUILD_TESTS)
    add_executable(arc-temporal-visibility-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/temporal_visibility_tests.cpp)
    target_link_libraries(arc-temporal-visibility-tests PRIVATE arc::core)
    add_test(NAME arc-temporal-visibility-tests COMMAND arc-temporal-visibility-tests)

    add_executable(arc-visual-importance-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/visual_importance_tests.cpp)
    target_link_libraries(arc-visual-importance-tests PRIVATE arc::core)
    add_test(NAME arc-visual-importance-tests COMMAND arc-visual-importance-tests)

    add_executable(arc-mega-e-scenario-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/mega_e_scenario_tests.cpp)
    target_link_libraries(arc-mega-e-scenario-tests PRIVATE arc::core)
    add_test(NAME arc-mega-e-scenario-tests COMMAND arc-mega-e-scenario-tests)
endif()

if(WIN32)
    add_executable(arc-mega-e-visual-fps WIN32
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/mega_e_visual_fps.cpp)
    target_link_libraries(arc-mega-e-visual-fps PRIVATE arc::core user32 gdi32)
    target_compile_features(arc-mega-e-visual-fps PRIVATE cxx_std_23)

    if(ARC_BUILD_TESTS AND ARC_GPU_TESTS)
        add_executable(arc-temporal-visibility-native
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/temporal_visibility_native.cpp)
        target_link_libraries(arc-temporal-visibility-native PRIVATE arc::core d3d12 dxgi d3dcompiler)
        add_test(NAME arc-temporal-visibility-native COMMAND arc-temporal-visibility-native)
        set_tests_properties(
            arc-temporal-visibility-native
            PROPERTIES LABELS gpu TIMEOUT 60 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    endif()
endif()
