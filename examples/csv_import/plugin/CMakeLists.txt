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

set(CSV_IMPORT_SDK "" CACHE PATH "Published CSV importer SDK directory")
if (NOT EXISTS "${CSV_IMPORT_SDK}/contract.json" OR NOT EXISTS "${CSV_IMPORT_SDK}/csv_contract.bindings.h" OR
    NOT EXISTS "${CSV_IMPORT_SDK}/receivers/import_progress.bindings.h" OR
    NOT EXISTS "${CSV_IMPORT_SDK}/services/import_status.bindings.h" OR
    NOT EXISTS "${CSV_IMPORT_SDK}/huxerui/pluginkit/plugin.h")
    message(FATAL_ERROR "Set CSV_IMPORT_SDK to the SDK published by the host build")
endif ()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CSV_IMPORT_SDK}/contract.json")
if (NOT WIN32)
    find_package(Threads REQUIRED)
endif ()
file(READ "${CSV_IMPORT_SDK}/contract.json" contract)
string(JSON contract_id GET "${contract}" id)
string(JSON contract_version GET "${contract}" version)
string(JSON contract_fingerprint GET "${contract}" fingerprint)

function(add_importer target source plugin_id contribution)
    add_library(${target} SHARED "${source}")
    set_source_files_properties("${source}" PROPERTIES OBJECT_DEPENDS "${CSV_IMPORT_SDK}/contract.json")
    target_include_directories(${target} PRIVATE "${CSV_IMPORT_SDK}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_link_libraries(${target} PRIVATE HuxerUI::huxerui)
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
    configure_file(plugin.json.in "${CMAKE_CURRENT_BINARY_DIR}/${target}.json" @ONLY)
    file(GENERATE OUTPUT "${package_dir}/plugin.json" INPUT "${CMAKE_CURRENT_BINARY_DIR}/${target}.json")
endfunction()

add_importer(csv-importer csv_importer.cpp example.csv csv)

option(CSV_IMPORT_TEST_FIXTURES "Build the worker failure fixture for the example verification" OFF)
if (CSV_IMPORT_TEST_FIXTURES)
    add_importer(csv-faults faults.cpp example.csv-faults faults)
endif ()
