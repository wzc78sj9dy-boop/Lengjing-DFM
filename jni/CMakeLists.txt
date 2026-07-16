cmake_minimum_required(VERSION 3.22)
project(lengjing VERSION 1.0.0 LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options("SHELL:-mllvm -irobf")

file(GLOB_RECURSE LENGJING_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER LENGJING_SOURCES EXCLUDE REGEX "/imgui_demo\\.cpp$")
list(APPEND LENGJING_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/t3/t3sdk.cpp"
)

add_executable(lengjing ${LENGJING_SOURCES})
set_target_properties(lengjing PROPERTIES
    OUTPUT_NAME lengjing
    POSITION_INDEPENDENT_CODE ON
)

target_compile_definitions(lengjing PRIVATE
    LENGJING_VERSION="${PROJECT_VERSION}"
)

target_include_directories(lengjing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Android_draw
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Android_Graphics
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Android_my_imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/include/Android_touch
    ${CMAKE_CURRENT_SOURCE_DIR}/include/My_Utils
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ImGui
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ImGui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/include/native_surface
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/embree4
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/paradise
)

target_compile_options(lengjing PRIVATE
    -fvisibility=hidden
    -fdata-sections
    -ffunction-sections
    $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-ffp-contract=fast>
    $<$<CONFIG:Release>:-fmerge-all-constants>
    $<$<CONFIG:Release>:-funroll-loops>
    $<$<CONFIG:Release>:-fomit-frame-pointer>
    $<$<CONFIG:Release>:-flto=thin>
)

target_link_options(lengjing PRIVATE
    -Wl,--gc-sections
    -Wl,--icf=safe
    -Wl,-O2
    $<$<CONFIG:Release>:-flto=thin>
    $<$<CONFIG:Release>:-Wl,--strip-all>
)

set(LENGJING_STATIC_LIBRARIES
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/paradise/libparadise_api.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/kernel/libkernel_modules.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libembree4.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/liblexers.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libmath.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsimd.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsys.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libtasking.a
)

foreach(LENGJING_LIBRARY IN LISTS LENGJING_STATIC_LIBRARIES)
    if(NOT EXISTS "${LENGJING_LIBRARY}")
        message(FATAL_ERROR "Required static library is missing: ${LENGJING_LIBRARY}")
    endif()
endforeach()

target_link_libraries(lengjing PRIVATE
    log
    android
    dl
    z
    ${LENGJING_STATIC_LIBRARIES}
)
