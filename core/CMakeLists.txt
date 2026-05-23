# ---------------------------------------------------------------------------
# core/ — the engine. Capture → convert → encode → mux.
# ---------------------------------------------------------------------------

set(GPUR_CORE_SOURCES
    d3d_context.cpp
    pipeline.cpp

    capture/wgc_capture.cpp
    capture/dda_capture.cpp
    capture/window_tracker.cpp

    convert/color_converter.cpp

    encode/nvenc_encoder.cpp
    encode/amf_encoder.cpp
    encode/qsv_encoder.cpp

    audio/wasapi_loopback.cpp
    audio/wasapi_mic.cpp
    audio/mixer.cpp
    audio/aac_encoder.cpp

    mux/ffmpeg_muxer.cpp
    mux/replay_buffer.cpp
)

add_library(gpur_core STATIC ${GPUR_CORE_SOURCES})

target_include_directories(gpur_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(gpur_core
    PUBLIC
        gpur_shared
        # Windows system libs
        d3d11
        dxgi
        dxguid
        d3dcompiler
        windowsapp        # WinRT (Windows Graphics Capture)
        runtimeobject
        mfplat
        mfuuid
        mf
        mfreadwrite
        wmcodecdspuuid     # MFT_CATEGORY_AUDIO_ENCODER + AAC MFT GUIDs
        ole32
        avrt              # multimedia thread priorities (WASAPI)
        ksuser
)

# FFmpeg (vendored via vcpkg's FindFFMPEG.cmake).
target_include_directories(gpur_core PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(gpur_core PRIVATE ${FFMPEG_LIBRARIES})

# Per-backend feature flags propagated as preprocessor definitions.
if(GPUR_BUILD_NVENC)
    target_compile_definitions(gpur_core PUBLIC GPUR_HAVE_NVENC=1)
    target_include_directories(gpur_core PUBLIC
        "${GPUR_NVENC_SDK_PATH}/Interface"
    )
else()
    target_compile_definitions(gpur_core PUBLIC GPUR_HAVE_NVENC=0)
endif()

if(GPUR_BUILD_AMF)
    target_compile_definitions(gpur_core PUBLIC GPUR_HAVE_AMF=1)
else()
    target_compile_definitions(gpur_core PUBLIC GPUR_HAVE_AMF=0)
endif()

if(GPUR_BUILD_QSV)
    target_compile_definitions(gpur_core PUBLIC GPUR_HAVE_QSV=1)
else()
    target_compile_definitions(gpur_core PUBLIC GPUR_HAVE_QSV=0)
endif()

# C++/WinRT requires /await:strict and some other niceties; MSVC already enables
# coroutines under /std:c++20 so nothing extra needed beyond the standard flag.

# ---------------------------------------------------------------------------
# Compile HLSL shaders to embedded byte arrays so the binary stays self-
# contained. Output: ${CMAKE_CURRENT_BINARY_DIR}/shaders/<name>.h
# ---------------------------------------------------------------------------

set(GPUR_SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${GPUR_SHADER_OUT_DIR}")

function(gpur_compile_hlsl hlsl_file entry profile var_name)
    get_filename_component(_base "${hlsl_file}" NAME_WE)
    set(_out_h "${GPUR_SHADER_OUT_DIR}/${_base}.h")

    add_custom_command(
        OUTPUT  "${_out_h}"
        COMMAND fxc.exe
                /nologo
                /T ${profile}
                /E ${entry}
                /Vn ${var_name}
                /Fh "${_out_h}"
                "${CMAKE_CURRENT_SOURCE_DIR}/${hlsl_file}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${hlsl_file}"
        COMMENT "FXC: compiling ${hlsl_file} (${profile}, entry=${entry})"
        VERBATIM
    )

    target_sources(gpur_core PRIVATE "${_out_h}")
endfunction()

gpur_compile_hlsl(convert/bgra_to_nv12.hlsl main cs_5_0 g_bgra_to_nv12_cs)

target_include_directories(gpur_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(GPUR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
