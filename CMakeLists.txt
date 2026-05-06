# ImGui-Terminal — cross-platform CMake build.
#
# Replaces the per-platform Makefiles. One source of truth, one build/
# directory, sane include/link paths everywhere.
#
# Quick start (all platforms):
#   cmake -B build -S .
#   cmake --build build [--config Release]   # --config only matters for
#                                            # multi-config generators
#                                            # (MSVC, Xcode); ignored
#                                            # by Make/Ninja
#
# Windows MSVC additionally needs vcpkg cloned in-repo before configure:
#   git clone https://github.com/microsoft/vcpkg
#   .\vcpkg\bootstrap-vcpkg.bat
# CMake auto-detects ./vcpkg/ — no toolchain flag needed. See README.md
# for per-platform package install commands.
#
# Targets:
#   imgui_terminal           — default GLFW + OpenGL host (always built)
#   test_core, test_input    — headless test harnesses (POSIX only)
#   imgui_terminal_metal     — Metal renderer host (macOS only)
#   imgui_terminal_vulkan    — Vulkan renderer host (built only if SDK found)
#   p_osx                    — native macOS Cocoa+Metal client (macOS only)
#
# All output lands at  <build_dir>/<target>[.exe]  — multi-config
# generators (MSBuild) are flattened so the test runners can rely on
# build/test_core regardless of Debug/Release.

# --- vcpkg auto-detection (must run BEFORE project()) ----------------
# Resolution order:
#   1. Caller already specified -DCMAKE_TOOLCHAIN_FILE=...   (respect it)
#   2. ./vcpkg/ exists in the repo                          (preferred)
#   3. VCPKG_ROOT env var is set                            (fallback)
#   4. Nothing — POSIX users without vcpkg get pkg-config below
#
# Rationale: dropping vcpkg next to the source means a Windows user
# clones the repo, runs `bootstrap-vcpkg.bat` once, then `cmake -B
# build -S .` Just Works — no -D flag, no env var.
#
# Gated on Windows specifically: a stray ./vcpkg/ on a Mac/Linux
# checkout (leftover from a Windows experiment, accidental clone)
# must NOT silently activate the vcpkg toolchain there — POSIX
# resolves deps via pkg-config and a vcpkg toolchain would change
# find_package() behavior in ways the rest of this file doesn't
# expect. Mac/Linux users who genuinely want vcpkg can pass
# -DCMAKE_TOOLCHAIN_FILE explicitly.

if(WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE
            "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain (auto-detected: ./vcpkg)")
    elseif(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain (from VCPKG_ROOT)")
    endif()
endif()

cmake_minimum_required(VERSION 3.21)
project(imgui_terminal LANGUAGES C CXX)

# Apple targets compile .mm sources, which need Objective-C++. Enable
# the language and configure ARC + deprecation suppression up-front so
# any Apple target later in the file works regardless of order. ARC is
# scoped to .mm sources only — applying -fobjc-arc to .cpp files is
# meaningless and produces compiler-warning churn on each build.
if(APPLE)
    enable_language(OBJCXX)
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/imgui/backends/imgui_impl_metal.mm"
        "${CMAKE_SOURCE_DIR}/imgui/backends/imgui_impl_osx.mm"
        "${CMAKE_SOURCE_DIR}/examples/example_mac_metal.mm"
        "${CMAKE_SOURCE_DIR}/platform/OSX/main.mm"
        PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc;-Wno-deprecated-declarations"
    )
endif()

#--------------------------------------------------------------------
# Language standards
#--------------------------------------------------------------------
# st.c is plain C99 (suckless inheritance). The C++ side requires
# C++20: config-imgui.h uses designated initializers, which gcc/clang
# allow as a GNU extension under C++11+ but MSVC enforces strictly
# (designated init landed in the standard with C++20). Apple needs
# Xcode 12+ (Apple clang 12+) to compile this.

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single-config generators (Make, Ninja) default to no build type,
# which means no optimization AND no debug symbols. Default to Release
# unless the user passed -DCMAKE_BUILD_TYPE=... or is on a multi-config
# generator (MSVC, Xcode — they pick at build time via --config).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

#--------------------------------------------------------------------
# Output directory — flatten multi-config to keep paths predictable
#--------------------------------------------------------------------
# By default MSBuild puts artifacts at build/Release/imgui_terminal.exe.
# Flatten so tests/automation/*/runner.sh's hardcoded build/test_core
# path works on every generator.

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}")
endforeach()

#--------------------------------------------------------------------
# config-imgui.h — generated once, then user-editable.
#--------------------------------------------------------------------
# Mirrors upstream st's config.h vs config.def.h pattern. Tracked file
# is config-imgui.def.h; the generated config-imgui.h is gitignored
# and customizations persist across rebuilds.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/config-imgui.h")
    configure_file(
        "${CMAKE_SOURCE_DIR}/config-imgui.def.h"
        "${CMAKE_SOURCE_DIR}/config-imgui.h"
        COPYONLY
    )
endif()

#--------------------------------------------------------------------
# Dependencies
#--------------------------------------------------------------------
# MSVC uses vcpkg manifest mode (vcpkg.json next to this file) and
# resolves via CONFIG mode find_package. Everything else — Linux,
# macOS, AND MinGW (which ships MSYS2 pkg-config) — uses pkg-config.
# We gate on MSVC, not WIN32, because WIN32 is true for both MSVC
# and MinGW but they need different dep-resolution paths. Both
# branches produce IMPORTED targets we link against uniformly via
# the IMW_*_TARGET variables.

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/imgui")

if(MSVC)
    find_package(glfw3      CONFIG REQUIRED)
    find_package(freetype   CONFIG REQUIRED)
    find_package(Fontconfig REQUIRED)         # Module mode (vcpkg ports it without a CONFIG file)
    set(IMW_GLFW_TARGET       glfw)
    set(IMW_FREETYPE_TARGET   Freetype::Freetype)
    set(IMW_FONTCONFIG_TARGET Fontconfig::Fontconfig)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3      REQUIRED IMPORTED_TARGET glfw3)
    pkg_check_modules(FREETYPE   REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
    set(IMW_GLFW_TARGET       PkgConfig::GLFW3)
    set(IMW_FREETYPE_TARGET   PkgConfig::FREETYPE)
    set(IMW_FONTCONFIG_TARGET PkgConfig::FONTCONFIG)
endif()

#--------------------------------------------------------------------
# Source-list constants — referenced by every target
#--------------------------------------------------------------------
# IMW_WIDGET     : the emulator + adapter (the actual product)
# IMW_IMGUI_CORE : ImGui core sources, always linked
# Backends and host shells are added per-target below.

set(IMW_WIDGET
    "${CMAKE_SOURCE_DIR}/st/st.c"
    "${CMAKE_SOURCE_DIR}/imgui_win.cpp"
)

set(IMW_IMGUI_CORE
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp"
)

#--------------------------------------------------------------------
# Per-target setup helper
#--------------------------------------------------------------------
# Every target wants the same include search paths, IMGUI_USER_CONFIG
# define, and platform-conditional warning/CRT flags. Centralise here
# so individual add_executable() blocks stay focused on what's
# different between targets.

function(imw_configure_target target)
    target_include_directories(${target} PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/st"
        "${IMGUI_DIR}"
        "${IMGUI_DIR}/backends"
        "${IMGUI_DIR}/misc/freetype"
    )
    target_compile_definitions(${target} PRIVATE
        IMGUI_USER_CONFIG="imgui_user_config.h"
    )
    if(MSVC)
        # Suckless-inherited C predates MSVC's secure-CRT push; we're
        # not migrating to *_s variants. /utf-8 because our literals
        # (test data, dump format) are UTF-8.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /utf-8)
    else()
        # Debug symbols are config-driven (CMake adds -g for Debug,
        # nothing for Release). Don't hardcode it here.
        target_compile_options(${target} PRIVATE -Wall -Wformat)
    endif()
    if(UNIX)
        # Enables BSD/POSIX legacy functions on glibc; no-op elsewhere.
        target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE)
    endif()
endfunction()

# Stage vcpkg's fontconfig data (fonts.conf + conf.d/) next to the
# binary on MSVC builds. fontconfig is built by vcpkg with absolute
# default paths pointing at vcpkg's install dir — the wrong location
# once the binary moves. imgui_win.cpp's imw_load_fonts() sets
# FONTCONFIG_PATH to <exe_dir>/fontconfig at runtime; this POST_BUILD
# copy makes that path real.
#
# Triplet-agnostic: ${VCPKG_TARGET_TRIPLET} is set by vcpkg's toolchain
# to whatever's appropriate (x64-windows, arm64-windows, x86-windows).
function(imw_msvc_stage_fontconfig target)
    if(MSVC AND DEFINED VCPKG_TARGET_TRIPLET)
        # vcpkg ships the runtime fontconfig data at etc/fonts/
        # (containing fonts.conf + conf.d/). The share/fontconfig/
        # path holds only metadata (copyright, .conf.avail templates,
        # CMake wrappers) — NOT the runtime config.
        set(_fc_src
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/etc/fonts")
        if(EXISTS "${_fc_src}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${_fc_src}"
                    "$<TARGET_FILE_DIR:${target}>/fontconfig"
                VERBATIM
            )
        endif()
    endif()
endfunction()

#====================================================================
# Default target — GLFW + OpenGL host
#====================================================================
add_executable(imgui_terminal
    ${IMW_WIDGET}
    ${IMW_IMGUI_CORE}
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
    "${CMAKE_SOURCE_DIR}/examples/main_example_glfw_gl.cpp"
)
imw_configure_target(imgui_terminal)
imw_msvc_stage_fontconfig(imgui_terminal)
target_link_libraries(imgui_terminal PRIVATE
    ${IMW_GLFW_TARGET}
    ${IMW_FREETYPE_TARGET}
    ${IMW_FONTCONFIG_TARGET}
)
if(WIN32)
    target_link_libraries(imgui_terminal PRIVATE opengl32 gdi32 imm32)
elseif(APPLE)
    target_link_libraries(imgui_terminal PRIVATE
        "-framework OpenGL" "-framework Cocoa"
        "-framework IOKit"  "-framework CoreVideo"
    )
else()  # Linux / BSD
    target_link_libraries(imgui_terminal PRIVATE GL util)
endif()

#====================================================================
# Test harnesses — headless, no GLFW, no GPU backend
#====================================================================
# Same emulator + widget as the production binaries; the harness main
# replaces the host shell. fontconfig + freetype are still needed
# because imgui_win.cpp's font loader uses them.
#
# POSIX only. The harness sources use forkpty / openpty / SIGCHLD /
# unistd.h directly with no Windows abstractions; on Windows the
# build CI just produces the GLFW app, not the harness binaries.

if(NOT WIN32)
    foreach(harness IN ITEMS core input)
        set(t "test_${harness}")
        add_executable(${t}
            ${IMW_WIDGET}
            ${IMW_IMGUI_CORE}
            "${CMAKE_SOURCE_DIR}/tests/automation/${harness}/${t}.cpp"
        )
        imw_configure_target(${t})
        target_link_libraries(${t} PRIVATE
            ${IMW_FREETYPE_TARGET}
            ${IMW_FONTCONFIG_TARGET}
        )
        if(UNIX AND NOT APPLE)
            target_link_libraries(${t} PRIVATE util)   # forkpty/openpty
        endif()
    endforeach()
endif()

#====================================================================
# Metal renderer host (macOS only)
#====================================================================
# Same widget, GLFW for windowing, Metal for the renderer instead of
# OpenGL. Demonstrates the renderer-agnostic principle.

if(APPLE)
    add_executable(imgui_terminal_metal
        ${IMW_WIDGET}
        ${IMW_IMGUI_CORE}
        "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_metal.mm"
        "${CMAKE_SOURCE_DIR}/examples/example_mac_metal.mm"
    )
    imw_configure_target(imgui_terminal_metal)
    target_link_libraries(imgui_terminal_metal PRIVATE
        ${IMW_GLFW_TARGET}
        ${IMW_FREETYPE_TARGET}
        ${IMW_FONTCONFIG_TARGET}
        "-framework Metal"   "-framework MetalKit" "-framework QuartzCore"
        "-framework Cocoa"   "-framework IOKit"    "-framework CoreVideo"
    )
endif()

#====================================================================
# Vulkan renderer host — only built if a Vulkan SDK is present
#====================================================================
# Cross-platform; we silently skip it when the SDK isn't installed
# rather than failing the whole configure.

find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    add_executable(imgui_terminal_vulkan
        ${IMW_WIDGET}
        ${IMW_IMGUI_CORE}
        "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
        "${CMAKE_SOURCE_DIR}/examples/example_glfw_vulkan.cpp"
    )
    imw_configure_target(imgui_terminal_vulkan)
    imw_msvc_stage_fontconfig(imgui_terminal_vulkan)
    target_link_libraries(imgui_terminal_vulkan PRIVATE
        ${IMW_GLFW_TARGET}
        ${IMW_FREETYPE_TARGET}
        ${IMW_FONTCONFIG_TARGET}
        Vulkan::Vulkan
    )
    if(APPLE)
        target_link_libraries(imgui_terminal_vulkan PRIVATE
            "-framework Cocoa" "-framework IOKit" "-framework CoreVideo"
        )
    elseif(UNIX)
        target_link_libraries(imgui_terminal_vulkan PRIVATE util)
    endif()
else()
    message(STATUS "Vulkan SDK not found — skipping imgui_terminal_vulkan target "
                   "(install LunarG VulkanSDK / vulkan-loader to enable)")
endif()

#====================================================================
# Native macOS client — Cocoa + Metal, no GLFW
#====================================================================
# Replaces GLFW with native Cocoa windowing while keeping the same
# widget + emulator. Demonstrates the cross-platform widget API
# is genuinely renderer- and host-agnostic.

if(APPLE)
    add_executable(p_osx MACOSX_BUNDLE
        ${IMW_WIDGET}
        ${IMW_IMGUI_CORE}
        "${IMGUI_DIR}/backends/imgui_impl_osx.mm"
        "${IMGUI_DIR}/backends/imgui_impl_metal.mm"
        "${CMAKE_SOURCE_DIR}/platform/OSX/main.mm"
    )
    imw_configure_target(p_osx)
    set_target_properties(p_osx PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST       "${CMAKE_SOURCE_DIR}/platform/OSX/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER   "org.suckless.imgui-terminal"
        MACOSX_BUNDLE_BUNDLE_NAME      "ImGui Terminal"
        MACOSX_BUNDLE_BUNDLE_VERSION   "0.9.3"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.9.3"
    )
    target_link_libraries(p_osx PRIVATE
        ${IMW_FREETYPE_TARGET}
        ${IMW_FONTCONFIG_TARGET}
        "-framework Cocoa"   "-framework Metal"   "-framework MetalKit"
        "-framework QuartzCore" "-framework GameController"
        "-framework IOKit"   "-framework CoreVideo"
    )
    # rgb.txt resolution: imgui_win.cpp searches <exe_dir>/rgb.txt as one
    # of its fallbacks. Drop a copy beside the binary inside the bundle
    # so the GUI-launched app (CWD=/) finds it without widget changes.
    add_custom_command(TARGET p_osx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_SOURCE_DIR}/rgb.txt"
            "$<TARGET_BUNDLE_CONTENT_DIR:p_osx>/MacOS/rgb.txt"
        VERBATIM
    )
endif()
