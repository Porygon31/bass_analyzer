cmake_minimum_required(VERSION 3.20)
project(BassAnalyzer VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- FFTW3 ---
set(FFTW3_DIR "" CACHE PATH "Path to FFTW3 prebuilt directory")
if(NOT FFTW3_DIR)
    message(FATAL_ERROR
        "Set FFTW3_DIR to your FFTW3 folder.\n"
        "  cmake -DFFTW3_DIR=C:/libs/fftw-3.3.5-dll64 ..\n"
        "Download from: https://www.fftw.org/install/windows.html"
    )
endif()

set(FFTW3_LIB "${FFTW3_DIR}/libfftw3f-3.lib")
set(FFTW3_DLL "${FFTW3_DIR}/libfftw3f-3.dll")

if(NOT EXISTS "${FFTW3_LIB}")
    message(FATAL_ERROR "FFTW import library not found: ${FFTW3_LIB}")
endif()

if(NOT EXISTS "${FFTW3_DLL}")
    message(FATAL_ERROR "FFTW DLL not found: ${FFTW3_DLL}")
endif()

# --- ImGui (fetched from GitHub) ---
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
)
FetchContent_MakeAvailable(imgui)

# --- ImGui library target ---
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC d3d11 d3dcompiler dxgi)

# ---- Common sources ----
set(BASS_SOURCES
    main.cpp
    audio_capture.cpp
    bass_detector.cpp
    dsp_utils.cpp
    pitch_detector.cpp
    gui_app.cpp
)

set(BASS_HEADERS
    audio_capture.h
    bass_detector.h
    dsp_utils.h
    pitch_detector.h
    gui_app.h
)

# --- Main executable (GUI) ---
add_executable(bass_analyzer WIN32
    ${BASS_SOURCES}
    ${BASS_HEADERS}
)

# --- Console executable ---
add_executable(bass_analyzer_console
    ${BASS_SOURCES}
)

# ---- Common include dirs ----
foreach(target bass_analyzer bass_analyzer_console)
    target_include_directories(${target} PRIVATE
        ${FFTW3_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # FFTW (NO AMBIGUITY)
    target_link_libraries(${target} PRIVATE "${FFTW3_LIB}")

    # Other deps
    target_link_libraries(${target} PRIVATE
        imgui_lib
        ole32
        avrt
        ksuser
        dwmapi
    )

    # Copy FFTW DLL to output
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFTW3_DLL}"
            $<TARGET_FILE_DIR:${target}>
    )
endforeach()

# Console-only define
target_compile_definitions(bass_analyzer_console PRIVATE BASS_CONSOLE_MODE=1)
