cmake_minimum_required(VERSION 3.25)

project(wallpaper_mp4_laptop VERSION 0.3.0 LANGUAGES CXX RC)

set(WALLPAPER_MP4_EDITION_VERSION "0.3.0-laptop.1")
configure_file(src/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)

add_executable(wallpaper-mp4-laptop WIN32
    src/main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)

target_compile_features(wallpaper-mp4-laptop PRIVATE cxx_std_20)
target_compile_definitions(wallpaper-mp4-laptop PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(wallpaper-mp4-laptop PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET wallpaper-mp4-laptop PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

target_link_libraries(wallpaper-mp4-laptop PRIVATE
    advapi32
    comdlg32
    gdi32
    mf
    mfplat
    mfplay
    mfuuid
    ole32
    propsys
    shell32
    user32
)
