cmake_minimum_required(VERSION 3.24)
project(CsvImportPlugin LANGUAGES CXX)

set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI SDK directory")
if (ANDROID)
    find_package(HuxerUI CONFIG REQUIRED COMPONENTS static PATHS "${HUXERUI_HOME}"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
else()
    find_package(HuxerUI CONFIG REQUIRED COMPONENTS shared HINTS "${HUXERUI_HOME}")
endif()

set(CSV_IMPORT_SDK "" CACHE PATH "Published CSV example SDK directory")
if (NOT EXISTS "${CSV_IMPORT_SDK}/PluginKit.cmake" OR NOT EXISTS "${CSV_IMPORT_SDK}/csv_contract.bindings.h")
    message(FATAL_ERROR "Set CSV_IMPORT_SDK to the SDK published by the host build")
endif ()
include("${CSV_IMPORT_SDK}/PluginKit.cmake")

function(add_csv_package target source plugin_id plugin_version resource_root)
    set(secondary_importer "")
    if (source STREQUAL "csv_importer.cpp")
        set(secondary_importer "\n    {\"point\": \"example.table-importers\", \"metadata\": {\"extensions\": [\".ssv\"]}},")
    endif ()
    add_library(${target} SHARED "${source}")
    hpk_add_plugin(${target} MANIFEST plugin.json.in
        RESOURCE_ROOT "${resource_root}" RESOURCE_NAMESPACE csv_resources)
    if (WIN32)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    endif ()
endfunction()

add_csv_package(csv-importer csv_importer.cpp example.csv 0.1.0 resources)

option(CSV_IMPORT_TEST_FIXTURES "Build the extra packages used by the CSV verification" OFF)
if (CSV_IMPORT_TEST_FIXTURES)
    add_csv_package(csv-importer-worker csv_importer.cpp example.csv-worker 0.1.0 resources)
    add_csv_package(csv-faults faults.cpp example.csv-faults 0.1.0 resources)
    add_csv_package(csv-faults-upgrade faults.cpp example.csv-faults 0.2.0 upgrade_resources)
    target_compile_definitions(csv-faults-upgrade PRIVATE CSV_IMPORT_UPGRADE)
endif ()
