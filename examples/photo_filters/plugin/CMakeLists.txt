cmake_minimum_required(VERSION 3.24)
project(PhotoStudioFilters LANGUAGES CXX)

set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI SDK directory")
if (ANDROID)
    find_package(HuxerUI CONFIG REQUIRED COMPONENTS static PATHS "${HUXERUI_HOME}"
        NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
else()
    find_package(HuxerUI CONFIG REQUIRED COMPONENTS shared HINTS "${HUXERUI_HOME}")
endif()

set(PHOTO_STUDIO_SDK "" CACHE PATH "SDK published by the Photo Studio preview")
if (NOT EXISTS "${PHOTO_STUDIO_SDK}/photo/image_filter.bindings.h")
    message(FATAL_ERROR "Set PHOTO_STUDIO_SDK to the sdk directory published by the preview build")
endif()
include("${PHOTO_STUDIO_SDK}/PluginKit.cmake")

function(add_photo_filter target label description transform)
    set(plugin_id "example.photo.${target}")
    add_library(${target} SHARED image_filter.cpp)
    target_compile_definitions(${target} PRIVATE PHOTO_TRANSFORM=${transform})
    set(execution_modes [=["in_process", "worker_process"]=])
    set(panel_extension "")
    if (target STREQUAL "warm")
        target_compile_definitions(${target} PRIVATE PHOTO_WARM_PANEL)
        set(execution_modes [=["in_process"]=])
        set(panel_extension [=[,
    {"point": "example.photo-filter-panels"}]=])
    endif()
    hpk_add_plugin(${target} MANIFEST plugin.json.in)
endfunction()

add_photo_filter(mono "Black & White" "Turn color into neutral tones." Monochrome)
add_photo_filter(warm "Warm Tone" "Lift reds and soften blues for an amber finish." WarmTone)
