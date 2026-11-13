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
    set(worker_destination .)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(host_entry platform/linux/main.cpp)
    set(worker_destination bin)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(host_entry platform/macos/main.cpp)
    set(worker_destination MacOS)
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

if (HUXERUI_LIBRARY_GRAPH_ONLY)
    return()
endif ()
if (NOT TARGET hpk)
    message(FATAL_ERROR "The CSV preview requires HPK_BUILD_GENERATOR=ON")
endif ()

set(sdk_dir "${CMAKE_CURRENT_BINARY_DIR}/sdk")
set(csv_framework_includes "$<TARGET_PROPERTY:HuxerUI::huxerui,INTERFACE_INCLUDE_DIRECTORIES>")
set(contract_header "${CMAKE_CURRENT_SOURCE_DIR}/src/csv_contract.h")
set(status_header "${CMAKE_CURRENT_SOURCE_DIR}/src/import_status.h")
set(progress_header "${CMAKE_CURRENT_SOURCE_DIR}/src/receivers/import_progress.h")
set(sdk_inputs "${contract_header}" "${status_header}" "${progress_header}" "${CMAKE_CURRENT_SOURCE_DIR}/src/csv_sdk.h")
set(sdk_outputs "${sdk_dir}/csv_contract.h" "${sdk_dir}/csv_contract.bindings.h"
    "${sdk_dir}/csv_contract.host.h" "${sdk_dir}/contract.json"
    "${sdk_dir}/receivers/import_progress.h" "${sdk_dir}/receivers/import_progress.bindings.h"
    "${sdk_dir}/receivers/import_progress.host.h" "${sdk_dir}/receivers/contract.json"
    "${sdk_dir}/services/import_status.h" "${sdk_dir}/services/import_status.bindings.h"
    "${sdk_dir}/services/import_status.host.h" "${sdk_dir}/services/contract.json" "${sdk_dir}/csv_sdk.h")
list(APPEND sdk_outputs "${sdk_dir}/cpp-abi.txt")
foreach (header IN ITEMS
        huxerui/pluginkit.h
        huxerui/pluginkit/contract.h
        huxerui/pluginkit/plugin.h
        huxerui/pluginkit/detail/abi.h
        huxerui/pluginkit/detail/invocation.h
        huxerui/pluginkit/detail/extension.h
        huxerui/pluginkit/detail/wire.h)
    list(APPEND sdk_inputs "${kit_root}/include/${header}")
    list(APPEND sdk_outputs "${sdk_dir}/${header}")
endforeach ()

add_custom_command(OUTPUT ${sdk_outputs}
    COMMAND "$<TARGET_FILE:hpk>" gen "${progress_header}"
        csv_import::ImportProgress example.import-progress 1 "${sdk_dir}/receivers" -- "-I${kit_root}/include"
        "-I$<JOIN:${csv_framework_includes},;-I>"
    COMMAND "$<TARGET_FILE:hpk>" gen "${contract_header}"
        csv_import::TableImporter example.table-importer 1 "${sdk_dir}" -- "-I${kit_root}/include"
        "-I$<JOIN:${csv_framework_includes},;-I>"
    COMMAND "$<TARGET_FILE:hpk>" gen "${status_header}"
        csv_import::ImportStatus example.import-status 1 "${sdk_dir}/services" -- "-I${kit_root}/include"
        "-I$<JOIN:${csv_framework_includes},;-I>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/csv_sdk.h" "${sdk_dir}/csv_sdk.h"
    COMMAND "$<TARGET_FILE:hpk-abi-stamp>" "${sdk_dir}/cpp-abi.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${kit_root}/include/huxerui" "${sdk_dir}/huxerui"
    DEPENDS hpk hpk-abi-stamp ${sdk_inputs}
    COMMAND_EXPAND_LISTS VERBATIM
)
add_custom_target(csv-import-sdk DEPENDS ${sdk_outputs})

set_source_files_properties("${host_source}" PROPERTIES OBJECT_DEPENDS "${sdk_outputs}")
add_dependencies(example_lib_plugin_kit csv-import-sdk hpk-worker)
target_include_directories(example_lib_plugin_kit PRIVATE "${sdk_dir}" src "${kit_root}/src")
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
set_target_properties(hpk-worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:example_lib_plugin_kit>"
)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(example_lib_plugin_kit PROPERTIES BUILD_RPATH "$ORIGIN")
endif ()
if (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_target(csv-worker-runtime
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:HuxerUI::huxerui>" "$<TARGET_FILE_DIR:example_lib_plugin_kit>"
        VERBATIM)
    add_dependencies(example_lib_plugin_kit csv-worker-runtime)
endif ()
huxerui_add_runtime_dependencies(example_lib_plugin_kit
    FILES "$<TARGET_FILE:hpk-worker>"
    DESTINATION "${worker_destination}"
)
huxerui_add_runtime_dependencies(example_lib_plugin_kit TARGETS HuxerUI::huxerui)
huxerui_configure_project_app(example_lib_plugin_kit)
