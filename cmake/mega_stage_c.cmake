target_sources(arc-core PRIVATE
    src/core/scene_understanding.cpp
    src/core/contextual_effect_model.cpp
    src/core/compatibility_guard.cpp
)

if(ARC_BUILD_TESTS)
    add_executable(arc-scene-understanding-tests
        tests/scene_understanding_tests.cpp)
    target_link_libraries(arc-scene-understanding-tests PRIVATE arc::core)
    add_test(NAME arc-scene-understanding-tests COMMAND arc-scene-understanding-tests)

    add_executable(arc-contextual-effect-model-tests
        tests/contextual_effect_model_tests.cpp)
    target_link_libraries(arc-contextual-effect-model-tests PRIVATE arc::core)
    add_test(NAME arc-contextual-effect-model-tests COMMAND arc-contextual-effect-model-tests)

    add_executable(arc-compatibility-guard-tests
        tests/compatibility_guard_tests.cpp)
    target_link_libraries(arc-compatibility-guard-tests PRIVATE arc::core)
    add_test(NAME arc-compatibility-guard-tests COMMAND arc-compatibility-guard-tests)
endif()
