cmake_minimum_required(VERSION 3.24)

if (CMAKE_HOST_APPLE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
        AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS deployment target")
endif ()

project(example_lib_plugin_kit VERSION 0.1.0 LANGUAGES NONE)

include("${CMAKE_CURRENT_SOURCE_DIR}/HuxerUIProject.cmake")
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

get_filename_component(kit_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(host_source "${CMAKE_CURRENT_SOURCE_DIR}/src/csv_host.cpp")
if (WIN32)
    set(host_entry platform/windows/main.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(host_entry platform/linux/main.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(host_entry platform/macos/main.cpp)
endif ()
huxerui_add_app(example_lib_plugin_kit
    SOURCES "${host_source}" ${host_entry}
    BUNDLE_NAME "Lib-PluginKit Preview"
    BUNDLE_IDENTIFIER "org.huxerui.lib.pluginkit.preview"
)
huxerui_use_library(example_lib_plugin_kit
    TARGET HuxerUI::PluginKit
    PATH "${kit_root}"
)

hpk_add_sdk(example_lib_plugin_kit HEADERS sdk OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdk")
hpk_use_worker(example_lib_plugin_kit)

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()
if (NOT HPK_BUILD_GENERATOR)
    message(FATAL_ERROR "The CSV preview requires HPK_BUILD_GENERATOR=ON")
endif ()
set(csv_framework_includes "$<TARGET_PROPERTY:HuxerUI::huxerui,INTERFACE_INCLUDE_DIRECTORIES>")
set(codec_check_src "${CMAKE_CURRENT_SOURCE_DIR}/src/codec_check")
set(codec_check_dir "${CMAKE_CURRENT_BINARY_DIR}/codec-check")
set(codec_check_files "${codec_check_dir}/.hpk-sdk-files")
set(codec_check_header_list "${codec_check_dir}/headers.txt")
set(codec_check_headers codec_record.h codec_mapped.h codec_mappings.h)
string(REPLACE ";" "\n" codec_check_names "${codec_check_headers}")
file(GENERATE OUTPUT "${codec_check_header_list}" CONTENT "${codec_check_names}\n" NEWLINE_STYLE UNIX)
list(TRANSFORM codec_check_headers PREPEND "${codec_check_src}/" OUTPUT_VARIABLE codec_check_inputs)

add_custom_command(OUTPUT "${codec_check_files}"
    BYPRODUCTS "${codec_check_dir}/codec_record.h" "${codec_check_dir}/codec_record.bindings.h"
        "${codec_check_dir}/codec_mapped.h" "${codec_check_dir}/codec_mapped.bindings.h"
        "${codec_check_dir}/codec_mappings.h"
    COMMAND "$<TARGET_FILE:hpk>" gen "${codec_check_src}" "${codec_check_dir}" "${codec_check_header_list}" --
        "-I${kit_root}/include" "-I$<JOIN:${csv_framework_includes},;-I>" -isystem "${codec_check_src}"
    DEPENDS hpk "${codec_check_header_list}" ${codec_check_inputs}
    COMMAND_EXPAND_LISTS VERBATIM
)
add_custom_target(csv-contract-check-sdk DEPENDS "${codec_check_files}")
add_dependencies(example_lib_plugin_kit csv-contract-check-sdk)
target_include_directories(example_lib_plugin_kit PRIVATE src "${kit_root}/src"
    "${codec_check_dir}")
target_link_libraries(example_lib_plugin_kit PRIVATE nlohmann_json::nlohmann_json)
if (WIN32)
    target_compile_definitions(example_lib_plugin_kit PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(example_lib_plugin_kit PRIVATE /W4 /permissive- /utf-8)
    set_target_properties(example_lib_plugin_kit PROPERTIES
        WIN32_EXECUTABLE FALSE
        HUXERUI_WINDOWS_CRT_ENTRY wmainCRTStartup
    )
endif ()
set_target_properties(example_lib_plugin_kit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(example_lib_plugin_kit PROPERTIES BUILD_RPATH "$ORIGIN")
endif ()
huxerui_configure_project_app(example_lib_plugin_kit)
