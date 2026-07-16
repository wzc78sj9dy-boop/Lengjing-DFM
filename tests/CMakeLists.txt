cmake_minimum_required(VERSION 3.22)
project(lengjing_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(lengjing_tests
    config_tests.cpp
    coordinate_reader_tests.cpp
    hud_map_tests.cpp
    kernel_module_catalog_tests.cpp
    player_tracking_tests.cpp
    position_resolver_tests.cpp
    presentation_rate_tests.cpp
    projectile_speed_tests.cpp
    runtime_tests.cpp
    threat_catalog_tests.cpp
    touch_transform_tests.cpp
    test_main.cpp
    ../jni/src/config/LocalConfig.cpp
    ../jni/src/game/GameRuntime.cpp
    ../jni/src/render/PresentationRateTracker.cpp
)

target_include_directories(lengjing_tests PRIVATE ../jni/include)

if(MSVC)
    target_compile_options(lengjing_tests PRIVATE /W4 /utf-8)
else()
    target_compile_options(lengjing_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
