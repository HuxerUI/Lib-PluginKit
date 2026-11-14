cmake_minimum_required(VERSION 3.24)
project(CsvImportPlugin LANGUAGES CXX)

set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI SDK directory")
find_package(HuxerUI CONFIG REQUIRED COMPONENTS shared HINTS "${HUXERUI_HOME}")

if (WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(package_target windows-x86_64-msvc)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"
        AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(package_target linux-x86_64-gnu)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64"
        AND (NOT CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"))
    set(package_target macos-arm64)
else ()
    message(FATAL_ERROR "The CSV plugin targets Windows x64 (MSVC), Linux x86_64 (glibc), and macOS arm64")
endif ()

set(CSV_IMPORT_SDK "" CACHE PATH "Published CSV example SDK directory")
if (NOT EXISTS "${CSV_IMPORT_SDK}/csv_contract.bindings.h" OR
    NOT EXISTS "${CSV_IMPORT_SDK}/exporters/export_contract.bindings.h" OR
    NOT EXISTS "${CSV_IMPORT_SDK}/cpp-abi.txt" OR NOT EXISTS "${CSV_IMPORT_SDK}/csv_sdk.h" OR
    NOT EXISTS "${CSV_IMPORT_SDK}/receivers/import_progress.bindings.h" OR
    NOT EXISTS "${CSV_IMPORT_SDK}/services/import_status.bindings.h" OR
    NOT EXISTS "${CSV_IMPORT_SDK}/huxerui/pluginkit/plugin.h")
    message(FATAL_ERROR "Set CSV_IMPORT_SDK to the SDK published by the host build")
endif ()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CSV_IMPORT_SDK}/cpp-abi.txt")
if (NOT WIN32)
    find_package(Threads REQUIRED)
endif ()
file(READ "${CSV_IMPORT_SDK}/cpp-abi.txt" cpp_abi)
if (NOT cpp_abi MATCHES "^[0-9A-Za-z.+:=_-]+$")
    message(FATAL_ERROR "The published CSV SDK has an invalid C++ ABI stamp")
endif ()

function(add_csv_package target source plugin_id plugin_version)
    add_library(${target} SHARED "${source}")
    target_include_directories(${target} PRIVATE "${CSV_IMPORT_SDK}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_link_libraries(${target} PRIVATE HuxerUI::huxerui)
    target_compile_definitions(${target} PRIVATE
        "HPK_SDK_ABI=\"$<IF:$<CONFIG:Debug>,debug,release>-${HuxerUI_VERSION}\"")
    set(package_dir "${CMAKE_CURRENT_BINARY_DIR}/packages/${target}/$<CONFIG>")
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME plugin
        PREFIX ""
    )
    if (WIN32)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${package_dir}"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/imports/${target}/$<CONFIG>"
        )
    else ()
        target_link_libraries(${target} PRIVATE Threads::Threads)
        set_target_properties(${target} PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            LIBRARY_OUTPUT_DIRECTORY "${package_dir}"
        )
    endif ()
    set(secondary_importer "")
    if (source STREQUAL "csv_importer.cpp")
        set(secondary_importer "\n    {\"point\": \"example.table-importers\", \"metadata\": {\"extensions\": [\".ssv\"]}},")
    endif ()
    set(manifest "${CMAKE_CURRENT_BINARY_DIR}/${target}.json")
    configure_file(plugin.json.in "${manifest}" @ONLY)
    file(GENERATE OUTPUT "${package_dir}/plugin.json" INPUT "${manifest}")
endfunction()

add_csv_package(csv-importer csv_importer.cpp example.csv 0.1.0)

option(CSV_IMPORT_TEST_FIXTURES "Build the extra packages used by the CSV verification" OFF)
if (CSV_IMPORT_TEST_FIXTURES)
    add_csv_package(csv-importer-worker csv_importer.cpp example.csv-worker 0.1.0)
    add_csv_package(csv-faults faults.cpp example.csv-faults 0.1.0)
    add_csv_package(csv-faults-upgrade faults.cpp example.csv-faults 0.2.0)
    target_compile_definitions(csv-faults-upgrade PRIVATE CSV_IMPORT_UPGRADE)
endif ()
