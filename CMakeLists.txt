cmake_minimum_required(VERSION 3.20)
project(momentum_menu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "momentum is x64 — configure with -A x64")
endif()

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)

add_library(momentum_menu SHARED
    src/dllmain.cpp
    src/hooks.cpp
    src/menu.cpp
    src/recorder.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(momentum_menu PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(momentum_menu PRIVATE d3d11 dxgi shell32 psapi)

set_target_properties(momentum_menu PROPERTIES
    OUTPUT_NAME "momentum_menu"
    PREFIX ""
)

# Static CRT — the injected DLL carries its own runtime, so it loads on ANY
# x64 Windows machine with NO Visual C++ Redistributable needed. Essential
# for distribution: whoever clones the repo can just run it.
set_property(TARGET momentum_menu PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    target_compile_options(momentum_menu PRIVATE /W3 /MP)
    target_compile_definitions(momentum_menu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN)
endif()
