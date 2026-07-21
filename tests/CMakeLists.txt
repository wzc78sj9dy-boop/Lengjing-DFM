cmake_minimum_required(VERSION 3.22)
project(lengjing_tests LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(lengjing_tests
    actor_frame_visit_set_tests.cpp
    actor_record_refresh_policy_tests.cpp
    actor_record_source_tests.cpp
    actor_record_resolver_tests.cpp
    algorithm_position_policy_tests.cpp
    aim_mode_policy_tests.cpp
    aim_prediction_tests.cpp
    auth_config_tests.cpp
    auth_session_tests.cpp
    auth_transport_tests.cpp
    card_input_policy_tests.cpp
    cloud_layout_startup_policy_tests.cpp
    cloud_layout_tests.cpp
    config_tests.cpp
    cpu_mailbox_policy_tests.cpp
    frame_projection_tests.cpp
    geometry_scene_build_policy_tests.cpp
    hud_map_tests.cpp
    kernel_module_catalog_tests.cpp
    overlay_contrast_policy_tests.cpp
    player_bounds_tests.cpp
    player_tracer_policy_tests.cpp
    player_tracking_tests.cpp
    position_resolver_tests.cpp
    presentation_rate_tests.cpp
    projectile_speed_tests.cpp
    render_backend_selection_tests.cpp
    remote_elf_identity_tests.cpp
    runtime_layout_override_tests.cpp
    runtime_exit_policy_tests.cpp
    runtime_presentation_policy_tests.cpp
    runtime_tests.cpp
    threat_catalog_tests.cpp
    tracking_calculator_tests.cpp
    touch_transform_tests.cpp
    world_object_refresh_policy_tests.cpp
    test_main.cpp
    ../jni/lib/t3/t3sdk.cpp
    ../jni/src/auth/CloudLayout.cpp
    ../jni/src/auth/RemoteAuth.cpp
    ../jni/src/config/LocalConfig.cpp
    ../jni/src/game/GameRuntime.cpp
    ../jni/src/game/native/RemoteElfIdentity.cpp
    ../jni/src/render/PresentationRateTracker.cpp
)

target_include_directories(lengjing_tests PRIVATE
    ../jni/include
    ../jni/lib
)

if(WIN32)
    target_link_libraries(lengjing_tests PRIVATE ws2_32 iphlpapi)
endif()

if(MSVC)
    target_compile_options(lengjing_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(lengjing_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME lengjing_tests COMMAND lengjing_tests)

add_executable(kernel_rpc_transport_tests
    kernel_rpc_transport_tests.cpp
    ../jni/src/game/native/KernelRpcTransport.cpp
)

target_include_directories(kernel_rpc_transport_tests PRIVATE
    ../jni/include
)

if(MSVC)
    target_compile_options(kernel_rpc_transport_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(kernel_rpc_transport_tests PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

add_test(
    NAME kernel_rpc_transport_tests
    COMMAND kernel_rpc_transport_tests
)

add_executable(thread_execution_context_provider_tests
    thread_execution_context_provider_tests.cpp
    ../jni/src/game/native/ThreadContextDeviceTransport.cpp
    ../jni/src/game/native/ThreadExecutionContextProvider.cpp
)

target_include_directories(thread_execution_context_provider_tests PRIVATE
    ../jni/include
)

if(MSVC)
    target_compile_options(thread_execution_context_provider_tests PRIVATE
        /W4 /utf-8
    )
else()
    target_compile_options(thread_execution_context_provider_tests PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

add_test(
    NAME thread_execution_context_provider_tests
    COMMAND thread_execution_context_provider_tests
)

add_executable(ptrace_execution_context_provider_tests
    ptrace_execution_context_provider_tests.cpp
    ../jni/src/game/native/PtraceExecutionContextProvider.cpp
    ../jni/src/game/native/ThreadExecutionContextProvider.cpp
)

target_include_directories(ptrace_execution_context_provider_tests PRIVATE
    ../jni/include
)

if(MSVC)
    target_compile_options(ptrace_execution_context_provider_tests PRIVATE
        /W4 /utf-8
    )
else()
    target_compile_options(ptrace_execution_context_provider_tests PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

add_test(
    NAME ptrace_execution_context_provider_tests
    COMMAND ptrace_execution_context_provider_tests
)

add_executable(pacga_operand_resolver_tests
    pacga_operand_resolver_tests.cpp
    ../jni/src/game/native/PacgaOperandResolver.cpp
)

target_include_directories(pacga_operand_resolver_tests PRIVATE
    ../jni/include
)

if(MSVC)
    target_compile_options(pacga_operand_resolver_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(pacga_operand_resolver_tests PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

add_test(
    NAME pacga_operand_resolver_tests
    COMMAND pacga_operand_resolver_tests
)
