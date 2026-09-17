target_sources(arc-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core/steam_library.cpp)

if(ARC_BUILD_TESTS)
    add_executable(arc-steam-library-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/steam_library_tests.cpp)
    target_link_libraries(arc-steam-library-tests PRIVATE arc::core)
    add_test(NAME arc-steam-library-tests COMMAND arc-steam-library-tests)
endif()

if(WIN32)
    add_executable(arc-game-monitor ${CMAKE_CURRENT_SOURCE_DIR}/src/tools/arc_game_monitor.cpp)
    target_link_libraries(arc-game-monitor PRIVATE dxgi)
    target_compile_features(arc-game-monitor PRIVATE cxx_std_23)

    add_executable(arc-launcher WIN32 ${CMAKE_CURRENT_SOURCE_DIR}/src/launcher/arc_launcher_main.cpp)
    target_link_libraries(arc-launcher PRIVATE arc::core comctl32 shell32 advapi32)
    target_compile_features(arc-launcher PRIVATE cxx_std_23)
endif()
