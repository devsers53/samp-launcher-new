cmake_minimum_required(VERSION 3.16)

if(NOT WIN32)
    message(FATAL_ERROR "SA-MP Launcher requires Windows.")
endif()

# No VCL / RAD Studio here: the launcher is a plain C++ / Win32 / Dear ImGui
# application built with MinGW-w64 (gcc / g++). Use the Ninja or MinGW Makefiles
# generator, e.g.:
#   cmake -S . -B build -G Ninja \
#     -DCMAKE_MAKE_PROGRAM=PATH_TO_NINJA \
#     -DCMAKE_C_COMPILER=PATH_TO_GCC -DCMAKE_CXX_COMPILER=PATH_TO_GXX
option(SAMP_STATIC "Link the MinGW runtimes statically" ON)

project(samp LANGUAGES CXX)

set(SAMP_IMGUI    "${CMAKE_CURRENT_SOURCE_DIR}/imgui")
set(SAMP_LAUNCHER "${CMAKE_CURRENT_SOURCE_DIR}/launcher")

set(SAMP_SOURCES
    ${SAMP_LAUNCHER}/main.cpp
    ${SAMP_LAUNCHER}/core.cpp
    # Dear ImGui (single translation units)
    ${SAMP_IMGUI}/imgui.cpp
    ${SAMP_IMGUI}/imgui_draw.cpp
    ${SAMP_IMGUI}/imgui_tables.cpp
    ${SAMP_IMGUI}/imgui_widgets.cpp
    ${SAMP_IMGUI}/backends/imgui_impl_win32.cpp
    ${SAMP_IMGUI}/backends/imgui_impl_dx11.cpp
)

set(SAMP_HEADERS
    ${SAMP_LAUNCHER}/core.h
)

add_executable(samp WIN32
    ${SAMP_SOURCES}
    ${SAMP_HEADERS}
)

target_include_directories(samp PRIVATE
    ${SAMP_LAUNCHER}
    ${SAMP_IMGUI}
    ${SAMP_IMGUI}/backends
)

target_compile_definitions(samp PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_features(samp PRIVATE cxx_std_17)

# Win32 system libraries used by the launcher and its core
target_link_libraries(samp PRIVATE
    d3d11
    dxgi
    d3dcompiler
    wininet
    ws2_32
    shell32
    shlwapi
    ole32
    uuid
    comctl32
    advapi32
    gdi32
    user32
    gdiplus
    dwmapi
)

if(SAMP_STATIC)
    target_link_options(samp PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# ---- resources: icon + manifest + version info --------------------------------
find_program(SAMP_WINDRES windres HINTS "C:/MinGW64/mingw64/bin")
set(SAMP_RES "${CMAKE_CURRENT_BINARY_DIR}/samp.res")
if(SAMP_WINDRES)
    add_custom_command(
        OUTPUT "${SAMP_RES}"
        COMMAND "${SAMP_WINDRES}" --include-dir "${CMAKE_CURRENT_SOURCE_DIR}/resource"
                -O coff -i "${CMAKE_CURRENT_SOURCE_DIR}/resource/samp.rc" -o "${SAMP_RES}"
        DEPENDS
            resource/samp.rc
            resource/samp.ico
            "${SAMP_WINDRES}"
        VERBATIM
    )
    add_custom_target(samp_resource DEPENDS "${SAMP_RES}")
    add_dependencies(samp samp_resource)
    target_link_options(samp PRIVATE "${SAMP_RES}")
else()
    message(WARNING "windres not found; the launcher icon/manifest will be omitted.")
endif()

source_group("Source Files" FILES ${SAMP_SOURCES})
source_group("Header Files" FILES ${SAMP_HEADERS})