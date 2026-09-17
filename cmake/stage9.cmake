target_sources(arc-core PRIVATE
    src/core/quality_admission.cpp
    src/core/global_action_arbiter.cpp
    src/core/unified_runtime_governor.cpp
)

if(ARC_BUILD_TESTS)
    add_executable(arc-quality-admission-tests tests/quality_admission_tests.cpp)
    target_link_libraries(arc-quality-admission-tests PRIVATE arc::core)
    add_test(NAME arc-quality-admission-tests COMMAND arc-quality-admission-tests)

    add_executable(arc-global-action-arbiter-tests tests/global_action_arbiter_tests.cpp)
    target_link_libraries(arc-global-action-arbiter-tests PRIVATE arc::core)
    add_test(NAME arc-global-action-arbiter-tests COMMAND arc-global-action-arbiter-tests)

    add_executable(arc-unified-runtime-governor-tests tests/unified_runtime_governor_tests.cpp)
    target_link_libraries(arc-unified-runtime-governor-tests PRIVATE arc::core)
    add_test(NAME arc-unified-runtime-governor-tests COMMAND arc-unified-runtime-governor-tests)
endif()
