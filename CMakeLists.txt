cmake_minimum_required(VERSION 3.25)

project(wallpaper_mp4 VERSION 0.3.0 LANGUAGES CXX)

add_executable(wallpaper-mp4 WIN32
    src/main.cpp
)

target_compile_features(wallpaper-mp4 PRIVATE cxx_std_20)
target_compile_definitions(wallpaper-mp4 PRIVATE
    NOMINMAX
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(wallpaper-mp4 PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET wallpaper-mp4 PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

target_link_libraries(wallpaper-mp4 PRIVATE
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
