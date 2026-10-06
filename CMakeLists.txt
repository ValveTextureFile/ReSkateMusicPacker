cmake_minimum_required(VERSION 3.20)
project(ReSkateMusicPacker LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Include directories
include_directories(
    src
    External
    External/imgui
    External/lz4
    External/miniz
    External/zstd/lib
)

# External dependencies (without /WX)
# 1. lz4
add_library(lz4 STATIC External/lz4/lz4.c)
target_include_directories(lz4 PUBLIC External/lz4)

# 2. miniz
add_library(miniz STATIC External/miniz/miniz.c)
target_include_directories(miniz SYSTEM PUBLIC External/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_ZLIB_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)

# 3. zstd
file(GLOB zstd_sources CONFIGURE_DEPENDS
    External/zstd/lib/common/*.c
    External/zstd/lib/compress/*.c
    External/zstd/lib/decompress/*.c
)
add_library(zstd STATIC ${zstd_sources})
target_include_directories(zstd SYSTEM PUBLIC External/zstd/lib)
target_compile_definitions(zstd PRIVATE ZSTD_LEGACY_SUPPORT=0 ZSTD_DISABLE_ASM)

# 4. imgui: Win32 + Direct3D 12 on Windows, SDL2 + its renderer elsewhere
add_library(imgui STATIC
    External/imgui/imgui.cpp
    External/imgui/imgui_draw.cpp
    External/imgui/imgui_tables.cpp
    External/imgui/imgui_widgets.cpp
)
target_include_directories(imgui SYSTEM PUBLIC External/imgui)
if(WIN32)
    target_sources(imgui PRIVATE
        External/imgui/backends/imgui_impl_dx12.cpp
        External/imgui/backends/imgui_impl_win32.cpp)
    target_compile_definitions(imgui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_link_libraries(imgui PUBLIC d3d12 dxgi d3dcompiler dwmapi)
else()
    # Release builds link SDL2 statically (RSMP_STATIC_SDL2=ON), so the app runs without SDL2 installed.
    option(RSMP_STATIC_SDL2 "Link SDL2 statically (needs SDL2's static library, e.g. built from source)" OFF)
    find_package(SDL2 REQUIRED)
    target_sources(imgui PRIVATE
        External/imgui/backends/imgui_impl_sdl2.cpp
        External/imgui/backends/imgui_impl_sdlrenderer2.cpp)
    if(RSMP_STATIC_SDL2)
        target_link_libraries(imgui PUBLIC SDL2::SDL2-static)
    else()
        target_link_libraries(imgui PUBLIC SDL2::SDL2)
    endif()
endif()

# 4b. ooz: the open-source Oodle decoder, for the game's CAS blocks where its DLL cannot load.
if(NOT WIN32)
    add_library(ooz STATIC External/ooz/kraken.cpp External/ooz/bitknit.cpp External/ooz/lzna.cpp)
    target_include_directories(ooz SYSTEM PRIVATE External/ooz External/ooz/simde)
    target_compile_definitions(ooz PRIVATE OOZ_BUILD_DLL=1)
    target_compile_options(ooz PRIVATE -w) # third-party code, built as upstream ships it
    set_target_properties(ooz PROPERTIES CXX_STANDARD 17)
endif()

# Warnings as errors: always with MSVC, on request with GCC/Clang (whose warning sets vary by version).
option(RSMP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors on GCC and Clang" OFF)

# Target configuration helper for our code (enforces strict /W4 /WX)
function(configure_strict_target target)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /permissive- /EHsc /Zc:preprocessor)
    else()
        # Partial designated initializers are idiomatic C++20 and MSVC does not flag them.
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
        if(RSMP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# 5. Engine Core / Frostbite / VFS Library
set(ENGINE_SOURCES
    src/Engine/Core/Json/json.cpp
    src/Engine/Resource/binary_bundle.cpp
    src/Engine/Resource/binary_io.cpp
    src/Engine/Resource/bundle_ref_table.cpp
    src/Engine/Resource/cas_codec.cpp
    src/Engine/Resource/ebx_document.cpp
    src/Engine/Resource/ebx_merge.cpp
    src/Engine/Resource/ebx_writer.cpp
    src/Engine/Resource/material_grid.cpp
    src/Engine/Resource/shader_lookup.cpp
    src/Engine/Resource/toc.cpp
    src/Engine/Vfs/native_db.cpp
    src/Engine/Vfs/game_archives.cpp
    src/Engine/Vfs/game_bundles.cpp
)
add_library(engine STATIC ${ENGINE_SOURCES})
configure_strict_target(engine)
if(NOT MSVC)
    target_compile_options(engine PRIVATE -Wno-unused-function) # a helper only MSVC builds of ReSkate use
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC 12+ false positive on BinaryWriter::seek's inlined vector resize (binary_bundle.cpp).
    target_compile_options(engine PRIVATE -Wno-array-bounds -Wno-stringop-overflow)
endif()
target_link_libraries(engine PUBLIC lz4 miniz zstd)
if(NOT WIN32)
    target_link_libraries(engine PRIVATE ooz)
endif()

# 6. Packer Core Library
add_library(music_packer_lib STATIC src/packer.cpp src/ffmpeg_fetch.cpp src/sha.cpp)
configure_strict_target(music_packer_lib)
target_link_libraries(music_packer_lib PUBLIC engine)
if(WIN32)
    target_sources(music_packer_lib PRIVATE src/artwork_win32.cpp src/platform_win32.cpp)
    target_link_libraries(music_packer_lib PUBLIC ole32 gdi32 windowscodecs winhttp shell32)
else()
    target_sources(music_packer_lib PRIVATE src/artwork_posix.cpp src/platform_posix.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(music_packer_lib PUBLIC Threads::Threads)
endif()

# 7. Main Application (unified CLI + GUI; WIN32 subsystem on Windows, an app bundle on macOS)
if(WIN32)
    add_executable(ReSkateMusicPacker WIN32
        src/main.cpp
        src/gui_renderer_win32.cpp
        src/file_dialog_win32.cpp
    )
    target_link_libraries(ReSkateMusicPacker PRIVATE
        music_packer_lib
        imgui
        windowscodecs
        shell32
        ole32
        dwmapi
        dxgi
        d3d12
    )
else()
    add_executable(ReSkateMusicPacker src/main.cpp src/gui_renderer_sdl.cpp)
    if(APPLE)
        # A .app, so Finder opens it as a window; the CLI is ReSkateMusicPacker.app/Contents/MacOS/ReSkateMusicPacker.
        set_target_properties(ReSkateMusicPacker PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "ReSkate Music Packer"
            MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.deckarddetribine.reskatemusicpacker"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "1.1.2"
            MACOSX_BUNDLE_BUNDLE_VERSION "1.1.2"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in")
        target_sources(ReSkateMusicPacker PRIVATE src/file_dialog_mac.mm)
        set_source_files_properties(src/file_dialog_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(ReSkateMusicPacker PRIVATE "-framework AppKit" "-framework UniformTypeIdentifiers")
    else()
        target_sources(ReSkateMusicPacker PRIVATE src/file_dialog_linux.cpp)
    endif()
    target_link_libraries(ReSkateMusicPacker PRIVATE music_packer_lib imgui)
endif()
configure_strict_target(ReSkateMusicPacker)

# 8. Regression Tests
add_executable(ReSkateMusicPackerTests test/packer_tests.cpp)
configure_strict_target(ReSkateMusicPackerTests)
target_link_libraries(ReSkateMusicPackerTests PRIVATE music_packer_lib)
enable_testing()
add_test(NAME music_packer_tests COMMAND ReSkateMusicPackerTests)
add_executable(ReSkateArtworkTests test/artwork_tests.cpp)
configure_strict_target(ReSkateArtworkTests)
target_link_libraries(ReSkateArtworkTests PRIVATE music_packer_lib)
add_test(NAME artwork_tests COMMAND ReSkateArtworkTests "${CMAKE_BINARY_DIR}/artwork-preview.png")

add_executable(ReSkateFfmpegTests test/ffmpeg_fetch_tests.cpp)
configure_strict_target(ReSkateFfmpegTests)
target_link_libraries(ReSkateFfmpegTests PRIVATE music_packer_lib)
add_test(NAME ffmpeg_fetch_tests COMMAND ReSkateFfmpegTests)

# Off Windows only: there the Oodle blocks go through ooz (on Windows, the game's DLL).
if(NOT WIN32)
    add_executable(ReSkateCasCodecTests test/cas_codec_tests.cpp)
    configure_strict_target(ReSkateCasCodecTests)
    target_link_libraries(ReSkateCasCodecTests PRIVATE music_packer_lib)
    add_test(NAME cas_codec_tests COMMAND ReSkateCasCodecTests "${CMAKE_SOURCE_DIR}/test/fixtures")
endif()
