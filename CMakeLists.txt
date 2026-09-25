cmake_minimum_required(VERSION 3.24)

project(ARC VERSION 0.1.0 LANGUAGES CXX)

set(ARC_MSVC_INCLUDE_PREFIX "" CACHE STRING "Override localized MSVC include diagnostic prefix")
if(MSVC AND ARC_MSVC_INCLUDE_PREFIX)
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${ARC_MSVC_INCLUDE_PREFIX}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ARC_BUILD_TESTS "Build ARC tests" ON)
option(ARC_GPU_TESTS "Run GPU-backed synthetic tests on this machine" OFF)
option(ARC_STRESS_TESTS "Build and register long-running adversarial tests" OFF)

add_library(arc-core STATIC
    src/core/clock.cpp
    src/core/session.cpp
    src/core/multi_session.cpp
    src/core/event_ring.cpp
    src/core/resource_graph.cpp
    src/core/descriptor_ledger.cpp
    src/core/frame_timing_window.cpp
    src/core/optimizer_session.cpp
    src/core/resource_semantics.cpp
    src/core/scene_understanding.cpp
    src/core/gpu_attribution.cpp
    src/core/residency.cpp
    src/core/residency_candidates.cpp
    src/core/texture_quality.cpp
    src/core/texture_quality_candidates.cpp
    src/core/memory_arbiter.cpp
    src/core/memory_planner.cpp
    src/core/transition_predictor.cpp
    src/core/live_runtime.cpp
    src/core/runtime_event_bridge.cpp
    src/core/runtime_coordinator.cpp
    src/core/runtime_integration.cpp
    src/core/trace_writer.cpp
    src/core/trace_reader.cpp
)
add_library(arc::core ALIAS arc-core)

target_include_directories(arc-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(arc-core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(arc-core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(arc-core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

add_executable(arc-trace src/tools/arc_trace_main.cpp)
target_link_libraries(arc-trace PRIVATE arc::core)
add_executable(arc-trace-viewer src/tools/trace_viewer.cpp)
target_link_libraries(arc-trace-viewer PRIVATE arc::core)

if(WIN32)
    add_executable(arc-mask-cost experiments/record-pack/mask_cost.cpp)
    add_executable(arc-packet-gpu experiments/record-pack-gpu/replay.cpp)
    target_compile_definitions(arc-packet-gpu PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(arc-packet-gpu PRIVATE d3d12 dxgi d3dcompiler)
    target_compile_definitions(arc-mask-cost PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(arc-mask-cost PRIVATE d3d12 dxgi)
    add_executable(arc-exact-sort-gpu experiments/exact-sort-gpu/bench.cpp)
    target_compile_definitions(arc-exact-sort-gpu PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(arc-exact-sort-gpu PRIVATE d3d12 dxgi d3dcompiler bcrypt)
    add_executable(arc-cpu-contract-fixture tests/cpu_contract_capture_fixture.cpp)
    target_compile_definitions(arc-cpu-contract-fixture PRIVATE UNICODE _UNICODE NOMINMAX)
    add_library(arc-cpu-contract-capture SHARED src/backends/cpu/contract_capture.cpp)
    target_include_directories(arc-cpu-contract-capture PRIVATE include)
    target_compile_definitions(arc-cpu-contract-capture PRIVATE UNICODE _UNICODE NOMINMAX)
    add_library(arc-quality-metrics SHARED src/tools/quality_metrics.cpp)
    add_executable(arc-dynamic-city samples/dynamic_city.cpp)
    target_compile_definitions(arc-dynamic-city PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(arc-dynamic-city PRIVATE d3d12 dxgi d3dcompiler user32 ole32 windowscodecs)
    add_executable(arc-changed-words-tests tests/changed_words_tests.cpp)
    target_include_directories(arc-changed-words-tests PRIVATE include)
    add_executable(arc-context-attempt-budget-tests tests/context_attempt_budget_tests.cpp)
    target_include_directories(arc-context-attempt-budget-tests PRIVATE include)
    add_executable(arc-cpu-task-probe src/tools/cpu_task_probe.cpp)
    target_include_directories(arc-cpu-task-probe PRIVATE include)
    target_compile_definitions(arc-cpu-task-probe PRIVATE UNICODE _UNICODE NOMINMAX)
    add_executable(arc-process-sample src/tools/process_sample.cpp)
    target_compile_definitions(arc-process-sample PRIVATE UNICODE _UNICODE NOMINMAX)
    add_library(arc-dx12-observer STATIC
        src/backends/dx12/dx12_observer.cpp
        src/backends/dx12/views.cpp
        src/backends/dx12/barriers.cpp
        src/backends/dx12/residency.cpp
        src/backends/dx12/runtime_backend.cpp
    )
    target_include_directories(arc-dx12-observer PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_link_libraries(arc-dx12-observer PUBLIC arc::core d3d12 dxgi)
    target_compile_features(arc-dx12-observer PUBLIC cxx_std_23)

    add_executable(arc-calibrate src/calibrate/arc_calibrate_main.cpp)
    target_link_libraries(arc-calibrate PRIVATE arc::core arc-dx12-observer dxgi cabinet)

    foreach(sample dx12-memory-pressure dx12-texture-pressure dx12-mixed-resources)
        add_executable(${sample} samples/observer_validation.cpp)
        target_compile_definitions(${sample} PRIVATE ARC_SAMPLE_NAME="${sample}")
        target_link_libraries(${sample} PRIVATE arc-dx12-observer d3dcompiler psapi)
    endforeach()
    add_executable(dx12-residency-lab samples/residency_lab.cpp)
    target_link_libraries(dx12-residency-lab PRIVATE arc-dx12-observer)
    add_executable(dx12-tiled-texture-lab samples/tiled_texture_lab.cpp)
    target_link_libraries(dx12-tiled-texture-lab PRIVATE arc-dx12-observer)
    add_executable(dx12-global-memory-lab samples/global_memory_lab.cpp)
    target_link_libraries(dx12-global-memory-lab PRIVATE arc-dx12-observer)
    add_executable(dx12-live-runtime-lab samples/live_runtime_lab.cpp)
    target_link_libraries(dx12-live-runtime-lab PRIVATE arc-dx12-observer)
    add_executable(dx12-runtime-event-bridge-lab samples/runtime_event_bridge_lab.cpp)
    target_link_libraries(dx12-runtime-event-bridge-lab PRIVATE arc-dx12-observer)
    add_executable(dx12-runtime-backend-lab samples/runtime_backend_lab.cpp)
    target_link_libraries(dx12-runtime-backend-lab PRIVATE arc-dx12-observer)
endif()

if(ARC_BUILD_TESTS)
    add_executable(arc-spatial-sensitivity-tests tests/spatial_sensitivity_tests.cpp)
    target_link_libraries(arc-spatial-sensitivity-tests PRIVATE arc::core)
    add_test(NAME arc-spatial-sensitivity-tests COMMAND arc-spatial-sensitivity-tests)
    add_executable(arc-async-benchmark-writer-tests tests/async_benchmark_writer_tests.cpp)
    add_test(NAME arc-async-benchmark-writer-tests COMMAND arc-async-benchmark-writer-tests)
    add_executable(arc-target-feedback-tests tests/target_feedback_tests.cpp)
    target_link_libraries(arc-target-feedback-tests PRIVATE arc::core)
    add_test(NAME arc-target-feedback-tests COMMAND arc-target-feedback-tests)
    add_executable(arc-optimizer-session-tests tests/optimizer_session_tests.cpp)
    target_link_libraries(arc-optimizer-session-tests PRIVATE arc::core)
    add_test(NAME arc-optimizer-session-tests COMMAND arc-optimizer-session-tests)
    enable_testing()
    add_executable(arc-worker-placement-search-tests tests/worker_placement_search_tests.cpp)
    target_link_libraries(arc-worker-placement-search-tests PRIVATE arc::core)
    add_test(NAME arc-worker-placement-search-tests COMMAND arc-worker-placement-search-tests)
    add_executable(arc-worker-placement-plan-tests tests/worker_placement_plan_tests.cpp)
    target_link_libraries(arc-worker-placement-plan-tests PRIVATE arc::core)
    add_test(NAME arc-worker-placement-plan-tests COMMAND arc-worker-placement-plan-tests)
    add_executable(arc-intercept-cpu-meter-tests tests/intercept_cpu_meter_tests.cpp)
    target_link_libraries(arc-intercept-cpu-meter-tests PRIVATE arc::core)
    add_test(NAME arc-intercept-cpu-meter-tests COMMAND arc-intercept-cpu-meter-tests)
    add_executable(arc-frame-timing-window-tests tests/frame_timing_window_tests.cpp)
    target_link_libraries(arc-frame-timing-window-tests PRIVATE arc::core)
    add_test(NAME arc-frame-timing-window-tests COMMAND arc-frame-timing-window-tests)
    add_executable(arc-descriptor-ledger-tests tests/descriptor_ledger_tests.cpp)
    target_link_libraries(arc-descriptor-ledger-tests PRIVATE arc::core)
    add_test(NAME arc-descriptor-ledger-tests COMMAND arc-descriptor-ledger-tests)
    add_executable(arc-gpu-attribution-tests tests/gpu_attribution_tests.cpp)
    target_link_libraries(arc-gpu-attribution-tests PRIVATE arc::core)
    add_test(NAME arc-gpu-attribution-tests COMMAND arc-gpu-attribution-tests)
    add_executable(arc-core-tests tests/core_tests.cpp)
    target_link_libraries(arc-core-tests PRIVATE arc::core)
    add_test(NAME arc-core-tests COMMAND arc-core-tests)

    add_executable(arc-resource-semantics-tests tests/resource_semantics_tests.cpp)
    target_link_libraries(arc-resource-semantics-tests PRIVATE arc::core)
    add_test(NAME arc-resource-semantics-tests COMMAND arc-resource-semantics-tests)

    add_executable(arc-scene-understanding-tests tests/scene_understanding_tests.cpp)
    target_link_libraries(arc-scene-understanding-tests PRIVATE arc::core)
    add_test(NAME arc-scene-understanding-tests COMMAND arc-scene-understanding-tests)

    add_executable(arc-residency-policy-tests tests/residency_policy_tests.cpp)
    target_link_libraries(arc-residency-policy-tests PRIVATE arc::core)
    add_test(NAME arc-residency-policy-tests COMMAND arc-residency-policy-tests)

    add_executable(arc-residency-autonomous-sim-tests tests/residency_autonomous_sim_tests.cpp)
    target_link_libraries(arc-residency-autonomous-sim-tests PRIVATE arc::core)
    add_test(NAME arc-residency-autonomous-sim-tests COMMAND arc-residency-autonomous-sim-tests)

    add_executable(arc-residency-invariant-stress tests/residency_invariant_stress.cpp)
    target_link_libraries(arc-residency-invariant-stress PRIVATE arc::core)
    add_test(NAME arc-residency-invariant-stress COMMAND arc-residency-invariant-stress)

    add_executable(arc-residency-workload-matrix-tests tests/residency_workload_matrix_tests.cpp)
    target_link_libraries(arc-residency-workload-matrix-tests PRIVATE arc::core)
    add_test(NAME arc-residency-workload-matrix-tests COMMAND arc-residency-workload-matrix-tests)

    add_executable(arc-texture-quality-policy-tests tests/texture_quality_policy_tests.cpp)
    target_link_libraries(arc-texture-quality-policy-tests PRIVATE arc::core)
    add_test(NAME arc-texture-quality-policy-tests COMMAND arc-texture-quality-policy-tests)

    add_executable(arc-texture-quality-invariant-stress tests/texture_quality_invariant_stress.cpp)
    target_link_libraries(arc-texture-quality-invariant-stress PRIVATE arc::core)
    add_test(NAME arc-texture-quality-invariant-stress COMMAND arc-texture-quality-invariant-stress)

    add_executable(arc-memory-arbiter-tests tests/memory_arbiter_tests.cpp)
    target_link_libraries(arc-memory-arbiter-tests PRIVATE arc::core)
    add_test(NAME arc-memory-arbiter-tests COMMAND arc-memory-arbiter-tests)

    add_executable(arc-memory-planner-tests tests/memory_planner_tests.cpp)
    target_link_libraries(arc-memory-planner-tests PRIVATE arc::core)
    add_test(NAME arc-memory-planner-tests COMMAND arc-memory-planner-tests)

    add_executable(arc-candidate-surface-tests tests/candidate_surface_tests.cpp)
    target_link_libraries(arc-candidate-surface-tests PRIVATE arc::core)
    add_test(NAME arc-candidate-surface-tests COMMAND arc-candidate-surface-tests)

    add_executable(arc-global-memory-scenario-tests tests/global_memory_scenario_tests.cpp)
    target_link_libraries(arc-global-memory-scenario-tests PRIVATE arc::core)
    add_test(NAME arc-global-memory-scenario-tests COMMAND arc-global-memory-scenario-tests)

    add_executable(arc-transition-predictor-tests tests/transition_predictor_tests.cpp)
    target_link_libraries(arc-transition-predictor-tests PRIVATE arc::core)
    add_test(NAME arc-transition-predictor-tests COMMAND arc-transition-predictor-tests)

    add_executable(arc-live-runtime-tests tests/live_runtime_tests.cpp)
    target_link_libraries(arc-live-runtime-tests PRIVATE arc::core)
    add_test(NAME arc-live-runtime-tests COMMAND arc-live-runtime-tests)

    add_executable(arc-runtime-event-bridge-tests tests/runtime_event_bridge_tests.cpp)
    target_link_libraries(arc-runtime-event-bridge-tests PRIVATE arc::core)
    add_test(NAME arc-runtime-event-bridge-tests COMMAND arc-runtime-event-bridge-tests)

    add_executable(arc-runtime-coordinator-tests tests/runtime_coordinator_tests.cpp)
    target_link_libraries(arc-runtime-coordinator-tests PRIVATE arc::core)
    add_test(NAME arc-runtime-coordinator-tests COMMAND arc-runtime-coordinator-tests)

    add_executable(arc-runtime-integration-tests tests/runtime_integration_tests.cpp)
    target_link_libraries(arc-runtime-integration-tests PRIVATE arc::core)
    add_test(NAME arc-runtime-integration-tests COMMAND arc-runtime-integration-tests)

    if(ARC_STRESS_TESTS)
        add_executable(arc-multisession-stress tests/multisession_stress.cpp)
        target_link_libraries(arc-multisession-stress PRIVATE arc::core)
        add_test(NAME arc-multisession-stress COMMAND arc-multisession-stress 3 2000000)
        set_tests_properties(arc-multisession-stress PROPERTIES LABELS stress TIMEOUT 300 RUN_SERIAL TRUE)
    endif()
    if(WIN32)
        add_executable(arc-dx12-view-tests tests/dx12_view_tests.cpp)
        target_link_libraries(arc-dx12-view-tests PRIVATE arc-dx12-observer)
        add_test(NAME arc-dx12-view-tests COMMAND arc-dx12-view-tests)
        if(ARC_GPU_TESTS)
            add_executable(arc-gpu-attribution-native tests/gpu_attribution_native.cpp)
            target_link_libraries(arc-gpu-attribution-native PRIVATE arc::core d3d12 dxgi)
            add_test(NAME arc-gpu-attribution-native COMMAND arc-gpu-attribution-native)
            set_tests_properties(arc-gpu-attribution-native PROPERTIES LABELS gpu TIMEOUT 60 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
            add_executable(arc-dx12-enhanced-tests tests/dx12_enhanced_tests.cpp)
            target_link_libraries(arc-dx12-enhanced-tests PRIVATE arc-dx12-observer)
            add_test(NAME arc-dx12-enhanced-tests COMMAND arc-dx12-enhanced-tests)
            set_tests_properties(arc-dx12-enhanced-tests PROPERTIES LABELS gpu TIMEOUT 60 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
            foreach(sample dx12-memory-pressure dx12-texture-pressure dx12-mixed-resources)
                add_test(NAME ${sample} COMMAND ${sample} full 30)
                set_tests_properties(${sample} PROPERTIES LABELS gpu TIMEOUT 60 RUN_SERIAL TRUE)
            endforeach()
            add_test(NAME dx12-residency-lab-baseline COMMAND dx12-residency-lab baseline 24 2)
            add_test(NAME dx12-residency-lab-arc COMMAND dx12-residency-lab arc 24 2)
            add_test(NAME dx12-residency-lab-auto COMMAND dx12-residency-lab auto 24 2)
            set_tests_properties(dx12-residency-lab-baseline dx12-residency-lab-arc dx12-residency-lab-auto PROPERTIES LABELS gpu TIMEOUT 90 RUN_SERIAL TRUE)
            add_test(NAME dx12-tiled-texture-lab COMMAND dx12-tiled-texture-lab)
            set_tests_properties(
                dx12-tiled-texture-lab
                PROPERTIES
                    LABELS gpu
                    TIMEOUT 120
                    SKIP_RETURN_CODE 77
                    RUN_SERIAL TRUE
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
            add_test(NAME dx12-global-memory-lab COMMAND dx12-global-memory-lab)
            set_tests_properties(
                dx12-global-memory-lab
                PROPERTIES
                    LABELS gpu
                    TIMEOUT 180
                    SKIP_RETURN_CODE 77
                    RUN_SERIAL TRUE
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
            add_test(NAME dx12-live-runtime-lab COMMAND dx12-live-runtime-lab)
            set_tests_properties(
                dx12-live-runtime-lab
                PROPERTIES
                    LABELS gpu
                    TIMEOUT 180
                    SKIP_RETURN_CODE 77
                    RUN_SERIAL TRUE
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
            add_test(NAME dx12-runtime-event-bridge-lab COMMAND dx12-runtime-event-bridge-lab)
            set_tests_properties(
                dx12-runtime-event-bridge-lab
                PROPERTIES
                    LABELS gpu
                    TIMEOUT 120
                    SKIP_RETURN_CODE 77
                    RUN_SERIAL TRUE
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
            add_test(NAME dx12-runtime-backend-lab COMMAND dx12-runtime-backend-lab)
            set_tests_properties(
                dx12-runtime-backend-lab
                PROPERTIES
                    LABELS gpu
                    TIMEOUT 120
                    SKIP_RETURN_CODE 77
                    RUN_SERIAL TRUE
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        endif()
    endif()
endif()

include(cmake/stage5.cmake)
include(cmake/mega_e.cmake)
include(cmake/mega_f.cmake)
include(cmake/mega_g.cmake)
include(cmake/perceptual_portability.cmake)
include(cmake/generic_dx12_probe.cmake)

if(ARC_BUILD_TESTS)
    add_executable(arc-test-assertions-tests tests/test_assertions_tests.cpp)
    add_executable(arc-lifecycle-tests tests/lifecycle_tests.cpp)
    target_link_libraries(arc-lifecycle-tests PRIVATE arc::core)
    add_test(NAME arc-lifecycle-tests COMMAND arc-lifecycle-tests)
    add_test(NAME arc-test-assertions-tests COMMAND arc-test-assertions-tests)

    find_package(Threads REQUIRED)
    add_executable(arc-wicked-telemetry-tests tests/wicked_telemetry_tests.cpp)
    target_link_libraries(arc-wicked-telemetry-tests PRIVATE Threads::Threads)
    add_test(NAME arc-wicked-telemetry-tests COMMAND arc-wicked-telemetry-tests)

    find_program(ARC_POWERSHELL NAMES pwsh powershell)
    if(ARC_POWERSHELL)
        add_test(NAME arc-mega-d-validation-tests COMMAND ${ARC_POWERSHELL}
            -NoProfile -ExecutionPolicy Bypass -File
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mega_d_validation_tests.ps1)
        add_test(NAME arc-wicked-audit-boundary-tests COMMAND ${ARC_POWERSHELL}
            -NoProfile -ExecutionPolicy Bypass -File
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/wicked_audit_boundary_tests.ps1)
        add_test(NAME arc-wicked-presentation-contract-tests COMMAND ${ARC_POWERSHELL}
            -NoProfile -ExecutionPolicy Bypass -File
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/wicked_presentation_contract_tests.ps1)
        add_test(NAME arc-stage15-validation-tests COMMAND ${ARC_POWERSHELL}
            -NoProfile -ExecutionPolicy Bypass -File
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/stage15_validation_tests.ps1)
    endif()

    # Tests use assert for contracts (and some exercise operations inside it).
    # Keep assertions in every test configuration without changing production
    # library optimization or its NDEBUG contract. Run after all stage includes.
    get_property(arc_build_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(arc_target IN LISTS arc_build_targets)
        get_target_property(arc_sources ${arc_target} SOURCES)
        foreach(arc_source IN LISTS arc_sources)
            if(arc_source MATCHES "(^|/)tests/.*\\.cpp$")
                if(MSVC)
                    target_compile_options(${arc_target} PRIVATE /UNDEBUG)
                else()
                    target_compile_options(${arc_target} PRIVATE -UNDEBUG)
                endif()
                break()
            endif()
        endforeach()
    endforeach()
endif()

if(ARC_BUILD_TESTS)
    add_executable(arc-shadow-reuse-tests tests/shadow_reuse_tests.cpp)
    target_include_directories(arc-shadow-reuse-tests PRIVATE include)
    target_compile_features(arc-shadow-reuse-tests PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(arc-shadow-reuse-tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(arc-shadow-reuse-tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME arc-shadow-reuse-tests COMMAND arc-shadow-reuse-tests)
endif()

if(ARC_BUILD_TESTS)
    foreach(name candidate_contract discovery_budget cheap_observer cpu_region cpu_runtime_cost)
        string(REPLACE "_" "-" target_name "${name}")
        add_executable(arc-${target_name}-tests tests/${name}_tests.cpp)
        target_link_libraries(arc-${target_name}-tests PRIVATE arc::core)
        if(MSVC)
            target_compile_options(arc-${target_name}-tests PRIVATE /UNDEBUG)
        else()
            target_compile_options(arc-${target_name}-tests PRIVATE -UNDEBUG)
        endif()
        add_test(NAME arc-${target_name}-tests COMMAND arc-${target_name}-tests)
    endforeach()
endif()
