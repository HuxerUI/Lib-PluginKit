cmake_minimum_required(VERSION 3.24)

if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(huxerui_pluginkit VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

set(kit_sources src/manager.cpp src/worker_request.cpp)
set(kit_private_includes "${CMAKE_CURRENT_SOURCE_DIR}/src")
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(APPEND kit_sources platform/windows/src/worker_process.cpp)
        list(APPEND kit_private_includes "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/src")
        set(sha256_library bcrypt)
        set(clang_target x86_64-pc-windows-msvc)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
            AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        list(APPEND kit_sources platform/linux/src/worker_process.cpp)
        list(APPEND kit_private_includes "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/src")
        find_package(OpenSSL REQUIRED COMPONENTS Crypto)
        set(sha256_library OpenSSL::Crypto)
        set(clang_target x86_64-pc-linux-gnu)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64"
            AND (NOT CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"))
        list(APPEND kit_sources platform/macos/src/worker_process.cpp)
        list(APPEND kit_private_includes "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/src")
        set(clang_target "arm64-apple-macos${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else ()
        message(FATAL_ERROR "Lib-PluginKit targets Windows x64 (MSVC), Linux x86_64 (glibc), and macOS arm64")
    endif ()
    if (NOT WIN32)
        list(APPEND kit_sources src/worker_posix.cpp)
        find_package(Threads REQUIRED)
    endif ()
endif ()

huxerui_add_library(huxerui_pluginkit SOURCES ${kit_sources})
add_library(HuxerUI::PluginKit ALIAS huxerui_pluginkit)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()

find_package(nlohmann_json 3.12 CONFIG QUIET GLOBAL)
if (NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif ()

target_include_directories(huxerui_pluginkit PRIVATE ${kit_private_includes} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_compile_features(huxerui_pluginkit PUBLIC cxx_std_20)
target_link_libraries(huxerui_pluginkit PRIVATE nlohmann_json::nlohmann_json ${sha256_library})

add_executable(hpk-worker tools/worker_main.cpp)
target_include_directories(hpk-worker PRIVATE include ${kit_private_includes})
target_compile_features(hpk-worker PRIVATE cxx_std_20)

if (WIN32)
    set(kit_compile_options /W4 /permissive- /utf-8)
    set(kit_compile_definitions WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
else ()
    set(kit_compile_options -Wall -Wextra -Wpedantic)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(kit_compile_definitions _GNU_SOURCE)
    endif ()
    target_link_libraries(huxerui_pluginkit PRIVATE Threads::Threads)
    target_link_libraries(hpk-worker PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif ()

option(HPK_BUILD_GENERATOR "Build the hpk contract generator" ON)
if (HPK_BUILD_GENERATOR)
    set(HPK_LLVM_ROOT "$ENV{HPK_LLVM_ROOT}" CACHE PATH "LLVM installation with libclang")
    find_program(HPK_CLANG_EXECUTABLE NAMES clang HINTS "${HPK_LLVM_ROOT}/bin" REQUIRED)
    file(REAL_PATH "${HPK_CLANG_EXECUTABLE}" clang_path)
    get_filename_component(llvm_bin "${clang_path}" DIRECTORY)
    get_filename_component(llvm_root "${llvm_bin}" DIRECTORY)
    find_path(HPK_LIBCLANG_INCLUDE_DIR clang-c/Index.h HINTS "${llvm_root}/include" REQUIRED)
    if (WIN32)
        find_library(HPK_LIBCLANG_LIBRARY NAMES libclang HINTS "${llvm_root}/lib" REQUIRED)
        find_file(HPK_LIBCLANG_DLL libclang.dll HINTS "${llvm_bin}" REQUIRED)
    else ()
        find_library(HPK_LIBCLANG_LIBRARY NAMES clang HINTS "${llvm_root}/lib" REQUIRED)
    endif ()
    execute_process(COMMAND "${HPK_CLANG_EXECUTABLE}" -print-resource-dir
        OUTPUT_VARIABLE clang_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    file(TO_CMAKE_PATH "${clang_resource_dir}" clang_resource_dir)

    add_executable(hpk tools/hpk.cpp)
    target_include_directories(hpk PRIVATE include ${kit_private_includes} "${HPK_LIBCLANG_INCLUDE_DIR}")
    target_compile_features(hpk PRIVATE cxx_std_20)
    target_compile_options(hpk PRIVATE ${kit_compile_options})
    target_compile_definitions(hpk PRIVATE ${kit_compile_definitions} HPK_CLANG_TARGET="${clang_target}"
        HPK_CLANG_EXECUTABLE="${HPK_CLANG_EXECUTABLE}" HPK_CLANG_RESOURCE_DIR="${clang_resource_dir}")
    target_link_libraries(hpk PRIVATE "${HPK_LIBCLANG_LIBRARY}" nlohmann_json::nlohmann_json ${sha256_library})
    if (WIN32)
        add_custom_command(TARGET hpk POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${HPK_LIBCLANG_DLL}" "$<TARGET_FILE_DIR:hpk>")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        if (IS_DIRECTORY "${CMAKE_OSX_SYSROOT}")
            set(clang_sysroot "${CMAKE_OSX_SYSROOT}")
        else ()
            execute_process(COMMAND xcrun --sdk macosx --show-sdk-path OUTPUT_VARIABLE clang_sysroot
                OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        endif ()
        target_compile_definitions(hpk PRIVATE HPK_CLANG_SYSROOT="${clang_sysroot}")
    endif ()
endif ()

foreach (target IN ITEMS huxerui_pluginkit hpk-worker)
    target_compile_definitions(${target} PRIVATE ${kit_compile_definitions})
    target_compile_options(${target} PRIVATE ${kit_compile_options})
endforeach ()
