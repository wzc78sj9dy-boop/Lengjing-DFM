cmake_minimum_required(VERSION 3.18)
project(lengjing VERSION 1.0.0 LANGUAGES C CXX)

option(LENGJING_ENABLE_RUNTIME_AUTH "Enable runtime T3 authentication" OFF)
option(LENGJING_ENABLE_PROJECTILE_TRACKING
    "Enable projectile tracking runtime and UI" OFF)
set(LENGJING_AUTH_VERSION "1000" CACHE STRING
    "Runtime authentication version number")
if(NOT LENGJING_AUTH_VERSION MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "LENGJING_AUTH_VERSION must contain decimal digits only")
endif()

set(LENGJING_T3_LOGIN_CODE "" CACHE STRING "Private T3 login call code")
set(LENGJING_T3_NOTICE_CODE "" CACHE STRING "Private T3 notice call code")
set(LENGJING_T3_VERSION_CODE "" CACHE STRING "Private T3 version call code")
set(LENGJING_T3_HEARTBEAT_CODE "" CACHE STRING "Private T3 heartbeat call code")
set(LENGJING_T3_APP_KEY "" CACHE STRING "Private T3 application key")
set(LENGJING_T3_RSA_PUBLIC_KEY "" CACHE STRING "Private T3 RSA public key")
set(LENGJING_T3_GET_VARIABLE_CODE "" CACHE STRING "Private T3 variable call code")
set(LENGJING_T3_LAYOUT_VALUE_ID "" CACHE STRING "Private T3 layout value id")
set(LENGJING_T3_LAYOUT_VALUE_NAME "" CACHE STRING "Private T3 layout value name")
set(LENGJING_T3_LAYOUT_PACKAGE "com.tencent.tmgp.dfm" CACHE STRING "Cloud layout package")
set(LENGJING_T3_LAYOUT_MODULE "libUE4.so" CACHE STRING "Cloud layout module")
set(LENGJING_T3_LAYOUT_BUILD_ID "" CACHE STRING "Cloud layout ELF build id")

set(LENGJING_T3_PRIVATE_CACHE_VARIABLES
    LENGJING_T3_LOGIN_CODE
    LENGJING_T3_NOTICE_CODE
    LENGJING_T3_VERSION_CODE
    LENGJING_T3_HEARTBEAT_CODE
    LENGJING_T3_APP_KEY
    LENGJING_T3_RSA_PUBLIC_KEY
    LENGJING_T3_GET_VARIABLE_CODE
    LENGJING_T3_LAYOUT_VALUE_ID
    LENGJING_T3_LAYOUT_VALUE_NAME
    LENGJING_T3_LAYOUT_PACKAGE
    LENGJING_T3_LAYOUT_MODULE
    LENGJING_T3_LAYOUT_BUILD_ID
)
mark_as_advanced(${LENGJING_T3_PRIVATE_CACHE_VARIABLES})
foreach(LENGJING_T3_PRIVATE_CACHE_VARIABLE IN LISTS LENGJING_T3_PRIVATE_CACHE_VARIABLES)
    if("${${LENGJING_T3_PRIVATE_CACHE_VARIABLE}}" MATCHES "\\)LJCFG\"")
        message(FATAL_ERROR
            "${LENGJING_T3_PRIVATE_CACHE_VARIABLE} contains the reserved raw-string delimiter")
    endif()
endforeach()

set(LENGJING_T3_LOGIN_REQUIRED_VARIABLES
    LENGJING_T3_LOGIN_CODE
    LENGJING_T3_NOTICE_CODE
    LENGJING_T3_VERSION_CODE
    LENGJING_T3_HEARTBEAT_CODE
    LENGJING_T3_APP_KEY
    LENGJING_T3_RSA_PUBLIC_KEY
)
set(LENGJING_T3_CLOUD_REQUIRED_VARIABLES
    LENGJING_T3_GET_VARIABLE_CODE
    LENGJING_T3_LAYOUT_VALUE_ID
    LENGJING_T3_LAYOUT_VALUE_NAME
    LENGJING_T3_LAYOUT_BUILD_ID
)

if(LENGJING_ENABLE_RUNTIME_AUTH)
    foreach(LENGJING_T3_REQUIRED_VARIABLE IN LISTS LENGJING_T3_LOGIN_REQUIRED_VARIABLES)
        if("${${LENGJING_T3_REQUIRED_VARIABLE}}" STREQUAL "")
            message(FATAL_ERROR
                "Runtime authentication requires ${LENGJING_T3_REQUIRED_VARIABLE}")
        endif()
    endforeach()
endif()

set(LENGJING_T3_CLOUD_CONFIG_PRESENT OFF)
foreach(LENGJING_T3_REQUIRED_VARIABLE IN LISTS LENGJING_T3_CLOUD_REQUIRED_VARIABLES)
    if(NOT "${${LENGJING_T3_REQUIRED_VARIABLE}}" STREQUAL "")
        set(LENGJING_T3_CLOUD_CONFIG_PRESENT ON)
    endif()
endforeach()
if(LENGJING_T3_CLOUD_CONFIG_PRESENT)
    if(NOT LENGJING_ENABLE_RUNTIME_AUTH)
        message(FATAL_ERROR
            "Cloud layout configuration requires LENGJING_ENABLE_RUNTIME_AUTH=ON")
    endif()
    foreach(LENGJING_T3_REQUIRED_VARIABLE IN LISTS LENGJING_T3_CLOUD_REQUIRED_VARIABLES)
        if("${${LENGJING_T3_REQUIRED_VARIABLE}}" STREQUAL "")
            message(FATAL_ERROR
                "Cloud layout configuration requires ${LENGJING_T3_REQUIRED_VARIABLE}")
        endif()
    endforeach()

    string(LENGTH "${LENGJING_T3_LAYOUT_BUILD_ID}" LENGJING_T3_BUILD_ID_LENGTH)
    math(EXPR LENGJING_T3_BUILD_ID_REMAINDER
        "${LENGJING_T3_BUILD_ID_LENGTH} % 2")
    if(LENGJING_T3_BUILD_ID_LENGTH LESS 8 OR
       LENGJING_T3_BUILD_ID_LENGTH GREATER 128 OR
       NOT LENGJING_T3_BUILD_ID_REMAINDER EQUAL 0 OR
       NOT "${LENGJING_T3_LAYOUT_BUILD_ID}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "LENGJING_T3_LAYOUT_BUILD_ID must be 8-128 lowercase hexadecimal characters with even length")
    endif()
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/auth")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AuthConfigPrivate.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/auth/AuthConfigPrivate.h"
    @ONLY
)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(
    "SHELL:-mllvm -irobf"
    "SHELL:-mllvm -irobf-strenc"
)

file(GLOB_RECURSE LENGJING_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(APPEND LENGJING_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/t3/t3sdk.cpp"
)
list(FILTER LENGJING_SOURCES EXCLUDE REGEX "/imgui_demo\\.cpp$")
if(NOT LENGJING_ENABLE_PROJECTILE_TRACKING)
    list(FILTER LENGJING_SOURCES EXCLUDE REGEX
        "/game/native/TrajectoryHook\\.cpp$")
endif()

set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Android_Graphics/CPUGraphics.cpp"
    PROPERTIES
        COMPILE_OPTIONS
            "-O3;-ffast-math;-ftree-vectorize;-fno-stack-protector"
)

add_executable(lengjing ${LENGJING_SOURCES})
set_target_properties(lengjing PROPERTIES
    OUTPUT_NAME lengjing
    POSITION_INDEPENDENT_CODE ON
)

target_compile_definitions(lengjing PRIVATE
    LENGJING_VERSION="${PROJECT_VERSION}"
    LENGJING_AUTH_VERSION="${LENGJING_AUTH_VERSION}"
    LENGJING_ENABLE_RUNTIME_AUTH=$<BOOL:${LENGJING_ENABLE_RUNTIME_AUTH}>
    LENGJING_ENABLE_PROJECTILE_TRACKING=$<BOOL:${LENGJING_ENABLE_PROJECTILE_TRACKING}>
    VK_USE_PLATFORM_ANDROID_KHR
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
)

target_include_directories(lengjing PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
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
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/curl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/embree4
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/paradise
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/capstone/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/unicorn/include
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
    $<$<CONFIG:Release>:-g0>
    $<$<CONFIG:Release>:-flto=thin>
)

target_link_options(lengjing PRIVATE
    -Wl,--gc-sections
    -Wl,--icf=safe
    -Wl,-O2
    $<$<CONFIG:Release>:-flto=thin>
)

set(LENGJING_DRIVER_ARCHIVE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/paradise/libparadise_api.a")
set(LENGJING_SILENT_DRIVER_ARCHIVE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/lib/libdriver_api_silent.a")
if(NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "CMAKE_OBJCOPY is required for the driver archive")
endif()
if(NOT EXISTS "${LENGJING_DRIVER_ARCHIVE}")
    message(FATAL_ERROR
        "Required static library is missing: ${LENGJING_DRIVER_ARCHIVE}")
endif()
add_custom_command(
    OUTPUT "${LENGJING_SILENT_DRIVER_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/generated/lib"
    COMMAND "${CMAKE_OBJCOPY}"
        --redefine-sym printf=lengjing_silent_printf
        --redefine-sym puts=lengjing_silent_puts
        "${LENGJING_DRIVER_ARCHIVE}"
        "${LENGJING_SILENT_DRIVER_ARCHIVE}"
    DEPENDS "${LENGJING_DRIVER_ARCHIVE}"
    VERBATIM
)
add_custom_target(lengjing_silent_driver_archive
    DEPENDS "${LENGJING_SILENT_DRIVER_ARCHIVE}")
add_dependencies(lengjing lengjing_silent_driver_archive)

set(LENGJING_STATIC_LIBRARIES
    ${LENGJING_SILENT_DRIVER_ARCHIVE}
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/kernel/libkernel_scripts.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libembree4.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/liblexers.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libmath.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsimd.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libsys.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/libtasking.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/unicorn/arm64-v8a/libunicorn.a
    ${CMAKE_CURRENT_SOURCE_DIR}/lib/capstone/arm64-v8a/libcapstone.a
)

foreach(LENGJING_LIBRARY IN LISTS LENGJING_STATIC_LIBRARIES)
    if(NOT "${LENGJING_LIBRARY}" STREQUAL
           "${LENGJING_SILENT_DRIVER_ARCHIVE}" AND
       NOT EXISTS "${LENGJING_LIBRARY}")
        message(FATAL_ERROR "Required static library is missing: ${LENGJING_LIBRARY}")
    endif()
endforeach()

target_link_libraries(lengjing PRIVATE
    log
    android
    EGL
    GLESv3
    dl
    m
    z
    ${LENGJING_STATIC_LIBRARIES}
)
