#===========================================================================#
# MechCommander 2 -- native 64-bit port build
#
# Replaces the original Visual Studio 2005 solution (Source/MechCmd2.sln,
# format 9.00) and its dependency on the February 2006 DirectX SDK.
#
# The MC2 game/engine sources are Microsoft's, distributed under the Shared
# Source Limited Permissive License (see EULA.md in the upstream drop). All
# Microsoft notices in those files are retained. This build system and
# everything under src/gameos/impl are new work.
#
# GameOS itself shipped as headers + a 32-bit x86 COFF static library, which
# no modern 64-bit or WebAssembly toolchain can link. It is therefore
# reimplemented from scratch against Microsoft's own published headers in
# src/gameos/include. That reimplementation is clean-room: it derives only
# from those headers.
#===========================================================================#

cmake_minimum_required(VERSION 3.21)
project(MechCommander2 CXX C)   # C is needed for zlib

#---------------------------------------------------------------------------#
# Hard requirement: 64-bit. The whole point of the port is leaving x86 behind.
#---------------------------------------------------------------------------#
if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT EMSCRIPTEN)
    message(FATAL_ERROR "MC2 port targets 64-bit only (got ${CMAKE_SIZEOF_VOID_P}-byte pointers).")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(MC2_GAME_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/src/game")
set(MC2_GAMEOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gameos")

#---------------------------------------------------------------------------#
# Shared compile settings.
#
# IMPORTANT: the configuration macros are applied globally rather than
# per-target, unlike the original .vcproj files. _ARMOR is layout-affecting --
# e.g. Stuff::MemoryBlockBase gains a Stuff::Signature base class under it --
# so a mixed setting across targets silently produces incompatible struct
# layouts. The VS2005 build got this right only because its configuration
# names happened to line up across every project.
#---------------------------------------------------------------------------#
#---------------------------------------------------------------------------#
# zlib.
#
# GameOS bundled zlib and shipped its HEADERS (src/gameos/include/zlib.h,
# v1.1.3) but not the sources, so compress2/uncompress were the last two
# unresolved non-GameOS symbols. They are not optional -- they sit on the data
# path: ffile.cpp reads .fst fastfile entries through uncompress (which gates
# M2), LogisticsData writes and reads SAVE GAMES through both, and Packet.cpp
# compresses network packets.
#
# Stock zlib 1.3.1 is used rather than a drop-in replacement because the shipped
# FinalBuild archives were written by the reference implementation, and this has
# to read them byte for byte. The compress2/uncompress signatures are unchanged
# since 1.1.3, so the old GameOS headers on the game's include path still match.
#
# Deliberately built as its own target with ONLY its own include directory: it
# must not see gameos/include/zlib.h, or it would compile against 1998 headers.
#---------------------------------------------------------------------------#
set(MC2_ZLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/zlib")

if(NOT EXISTS "${MC2_ZLIB_DIR}/deflate.c")
    message(FATAL_ERROR
        "zlib sources not found at ${MC2_ZLIB_DIR}. "
        "Fetch with: git clone --depth 1 --branch v1.3.1 https://github.com/madler/zlib.git")
endif()

add_library(mc2_zlib STATIC
    "${MC2_ZLIB_DIR}/adler32.c"  "${MC2_ZLIB_DIR}/compress.c" "${MC2_ZLIB_DIR}/crc32.c"
    "${MC2_ZLIB_DIR}/deflate.c"  "${MC2_ZLIB_DIR}/infback.c"  "${MC2_ZLIB_DIR}/inffast.c"
    "${MC2_ZLIB_DIR}/inflate.c"  "${MC2_ZLIB_DIR}/inftrees.c" "${MC2_ZLIB_DIR}/trees.c"
    "${MC2_ZLIB_DIR}/uncompr.c"  "${MC2_ZLIB_DIR}/zutil.c"
)
target_include_directories(mc2_zlib PRIVATE "${MC2_ZLIB_DIR}")
if(MSVC)
    target_compile_definitions(mc2_zlib PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    target_compile_options(mc2_zlib PRIVATE /W0)
endif()

add_library(mc2_common INTERFACE)

target_include_directories(mc2_common INTERFACE
    "${MC2_GAME_DIR}"
    "${MC2_GAME_DIR}/Code"
    "${MC2_GAME_DIR}/MCLib"
    "${MC2_GAME_DIR}/MCLib/MLR"
    "${MC2_GAME_DIR}/MCLib/Stuff"
    "${MC2_GAME_DIR}/MCLib/GOSFX"
    "${MC2_GAME_DIR}/GUI"
    "${MC2_GAMEOS_DIR}/include"
)

if(NOT WIN32)
    # PORT: resolve legacy <windows.h> includes to the deliberately small host
    # compatibility surface; native builds continue to use the Windows SDK.
    target_include_directories(mc2_common BEFORE INTERFACE
        "${MC2_GAMEOS_DIR}/compat"
    )
endif()

target_compile_definitions(mc2_common INTERFACE
    NDEBUG
    FINAL                       # tested only in Code/*.cpp, never in headers
    USE_PROTOTYPES
    STRICT
    # NOMINMAX is deliberately NOT defined on Windows. Legacy MCLib calls
    # unqualified max()/min() at 60 live sites. The non-Windows shim supplies
    # global function templates rather than macros because the current tree
    # also has 32 qualified std::min/std::max uses in the port renderer.
    # _ARMOR / LAB_ONLY deliberately NOT defined -- see note above.
    _CRT_SECURE_NO_WARNINGS     # ~1963 strcpy/sprintf/strcat sites; noise only
    _CRT_NONSTDC_NO_DEPRECATE
    MC2_PORT_64BIT              # historical marker for the modern port shims
)

if(EMSCRIPTEN)
    # PORT: The shipped sources rely on MSVC's template lookup and declaration
    # extensions. Emscripten Clang supports those source semantics directly;
    # keeping them here avoids invasive edits to the legacy container library.
    target_compile_options(mc2_common INTERFACE
        -fms-extensions
        -fdelayed-template-parsing
        "SHELL:-include ${MC2_GAMEOS_DIR}/compat/emscripten_compat.h"
        -Wno-ignored-attributes
        -Wno-nonportable-include-path
    )
    target_compile_definitions(mc2_common INTERFACE MC2_LEGACY_WIN32_ABI)

    # PORT: OPTIMISE THE BROWSER BUILD. build.ps1's -Emscripten path defaults to
    # Debug, so every browser artifact ever shipped was -O0 and ran roughly an
    # order of magnitude slower than it needed to. Measured 2026-08-24: building
    # sixteen fonts for the LOAD GAME dialog cost 25 s of pure wasm CPU.
    #
    # The link stage is optimised too (see the MechCmd2 link options, C1).
    # Measured 2026-09-25: link -O2 did not change mission load (~7.6 s) or
    # quickload (~5.8 s), so the compile-time -O2 already carries the speed.
    target_compile_options(mc2_common INTERFACE -O2)
endif()

if(WIN32)
    # PORT: do not advertise a Windows host to Emscripten. The original drop
    # defined these in every native configuration only.
    target_compile_definitions(mc2_common INTERFACE WIN32 _WINDOWS)
endif()

if(MSVC)
    # Warnings are dialled down for now: the goal of milestone 1 is to find
    # hard errors, and 2005-era code at /W3 buries them in thousands of
    # signed/unsigned and truncation notes. This gets raised once it links.
    target_compile_options(mc2_common INTERFACE /W0 /EHsc /GR)

    # PORT: use the compiler's distinct wchar_t type. The legacy UTF-16 string
    # layer owns a fixed-width code-unit type instead of depending on MSVC's
    # removed /Zc:wchar_t- compatibility mode.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(mc2_common INTERFACE
            -Wno-unknown-pragmas        # MSVC #pragma warning(disable: N)
            -Wno-microsoft
            # Report every error per file, not the first 20. During the
            # modernization pass we want the whole error population in one
            # run so fixes can be batched by class instead of by file.
            -ferror-limit=0
        )
    endif()
endif()

#---------------------------------------------------------------------------#
# Source lists, generated from the original .vcproj files by
# tools/gen-source-lists.ps1 (the .vcproj files are the authoritative record
# of what the shipped build actually compiled; globbing would over-collect).
#---------------------------------------------------------------------------#
include(cmake/sources-stuff.cmake)
include(cmake/sources-mlr.cmake)
include(cmake/sources-gosfx.cmake)
include(cmake/sources-mclib.cmake)
include(cmake/sources-mechcmd2.cmake)

#---------------------------------------------------------------------------#
# GameOS reimplementation (clean-room, from src/gameos/include only).
#---------------------------------------------------------------------------#
# The backend is organised by seam, one directory per swappable subsystem, so a
# second implementation (SDL2 platform, WebGPU renderer, OPFS vfs) is a sibling
# rather than a rewrite of the file it replaces. See docs/ARCHITECTURE.md.
#
# NOTE the globs are deliberately one level deep, NOT recursive. A backend that
# needs different compile settings from the rest of GameOS lives one directory
# further down and gets its own target -- impl/renderer/webgpu/ is the first such
# case (it needs C++20; see below).
file(GLOB MC2_GAMEOS_SOURCES CONFIGURE_DEPENDS
    "${MC2_GAMEOS_DIR}/impl/*.cpp"
    "${MC2_GAMEOS_DIR}/impl/renderer/*.cpp"
    "${MC2_GAMEOS_DIR}/impl/audio/*.cpp"
    # PORT: M6-4 cut the audio seam. The portable layer and the selector live in
    # impl/audio; the null peer is always built because the strict regression
    # runs against it. Device peers are added per platform below.
    "${MC2_GAMEOS_DIR}/impl/audio/null/*.cpp"
    "${MC2_GAMEOS_DIR}/impl/platform/*.cpp"
    # PORT: M8-B7 in-process page guard. Always built, inert unless
    # MC2_PAGE_GUARD is set, so it costs nothing in a normal run and needs no
    # machine-wide settings to arm.
    "${MC2_GAMEOS_DIR}/impl/mem/*.cpp"
    "${MC2_GAMEOS_DIR}/impl/vfs/*.cpp"
    # PORT: M8-1 video seam. video_null.cpp is ALWAYS built -- with no decoder
    # the game behaves exactly as it has since Bink was stripped, which is what
    # keeps the strict bar (a null-renderer build) unmoved while video work is
    # in progress. The ffmpeg peer is opt-in, added below.
    "${MC2_GAMEOS_DIR}/impl/video/video_null.cpp"
)

#---------------------------------------------------------------------------#
# M8-1: ffmpeg video decoding -- optional, and OFF by default.
#
# One dependency covers WebM demux, VP9 and Opus, which is why it was taken over
# libvpx + libopus + libwebm. Linked DYNAMICALLY on purpose: that keeps the LGPL
# simple, and a DLL beside the exe is already how this port ships
# d3dcompiler_47.dll. Point MC2_FFMPEG_DIR at an LGPL shared build containing
# include/ and lib/.
#---------------------------------------------------------------------------#
set(MC2_FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ffmpeg"
    CACHE PATH "Root of an ffmpeg shared build with include/ and lib/")

# Default ON when the libraries are actually present and OFF otherwise, so a
# tree without them still configures and builds -- the null decoder covers it.
if(EXISTS "${MC2_FFMPEG_DIR}/include/libavcodec/avcodec.h")
    option(MC2_ENABLE_FFMPEG "Build the ffmpeg video decoder (WebM/VP9/Opus)" ON)
else()
    option(MC2_ENABLE_FFMPEG "Build the ffmpeg video decoder (WebM/VP9/Opus)" OFF)
endif()

# The checked-in ffmpeg package is a Windows DLL/import-library build and cannot
# be linked into wasm. Browser movie playback uses the browser media peer; keep
# the native decoder out even when a prior CMake cache recorded the option ON.
if(EMSCRIPTEN)
    set(MC2_ENABLE_FFMPEG OFF CACHE BOOL
        "Build the ffmpeg video decoder (WebM/VP9/Opus)" FORCE)
endif()

if(MC2_ENABLE_FFMPEG)
    if(NOT EXISTS "${MC2_FFMPEG_DIR}/include/libavcodec/avcodec.h")
        message(FATAL_ERROR
            "MC2_ENABLE_FFMPEG is ON but no ffmpeg headers at ${MC2_FFMPEG_DIR}/include. "
            "Point MC2_FFMPEG_DIR at an LGPL shared build.")
    endif()
    list(APPEND MC2_GAMEOS_SOURCES "${MC2_GAMEOS_DIR}/impl/video/video_ffmpeg.cpp")
endif()

# M8-B76: the browser decodes the .webm movies itself (a hidden <video> element
# for the picture, decodeAudioData for the track), so it gets that peer in place
# of the null decoder. Native is untouched.
if(EMSCRIPTEN)
    list(REMOVE_ITEM MC2_GAMEOS_SOURCES "${MC2_GAMEOS_DIR}/impl/video/video_null.cpp")
    list(APPEND MC2_GAMEOS_SOURCES "${MC2_GAMEOS_DIR}/impl/video/video_web.cpp")
endif()

#---------------------------------------------------------------------------#
# WebGPU (Dawn) renderer backend -- optional.
#
# OFF by default so the null-renderer build stays fast and dependency-free;
# headless bring-up and the unit harnesses do not need a GPU.
#
# CRT WARNING: Dawn is built Release (/MD). A Debug build of this project uses
# /MDd, and mixing CRTs puts two heaps in one process -- memory allocated by one
# freed by the other, corruption that looks nothing like its cause. build.ps1
# -WebGPU forces RelWithDebInfo for exactly this reason; do not defeat it.
#---------------------------------------------------------------------------#
option(MC2_ENABLE_WEBGPU "Build the Dawn/WebGPU renderer backend" OFF)

set(MC2_DAWN_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/dawn")
set(MC2_DAWN_BUILD "${MC2_DAWN_DIR}/out/Release")

if(MC2_ENABLE_WEBGPU)
    if(EMSCRIPTEN)
        # PORT: M5-5 uses Dawn's in-tree emdawnwebgpu target.  It supplies the
        # generated browser WebGPU headers plus the JavaScript binding library;
        # no native Dawn archive is meaningful in a wasm link.
        set(DAWN_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
        set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
        set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(DAWN_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        # Dawn 2026 emits an optional C++ module interface. Preserve its
        # required standard while restoring MC2's C++14 default afterward.
        set(MC2_SAVED_CXX_STANDARD "${CMAKE_CXX_STANDARD}")
        set(CMAKE_CXX_STANDARD 20)
        add_subdirectory("${MC2_DAWN_DIR}" "${CMAKE_BINARY_DIR}/dawn" EXCLUDE_FROM_ALL)
        set(CMAKE_CXX_STANDARD "${MC2_SAVED_CXX_STANDARD}")
        set(MC2_DAWN_RENDERER_LIB emdawnwebgpu_cpp)
        message(STATUS "MC2: browser WebGPU backend ON (emdawnwebgpu)")
    elseif(NOT EXISTS "${MC2_DAWN_BUILD}")
        message(FATAL_ERROR
            "MC2_ENABLE_WEBGPU=ON but Dawn is not built at ${MC2_DAWN_BUILD}.\n"
            "  cmake -S third_party/dawn -B third_party/dawn/out/Release -G Ninja "
            "-DCMAKE_BUILD_TYPE=Release -DDAWN_FETCH_DEPENDENCIES=OFF "
            "-DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DTINT_BUILD_TESTS=OFF\n"
            "  cmake --build third_party/dawn/out/Release --target webgpu_dawn")
    else()

    # Dawn generates its C and C++ headers into the build tree, so both the
    # source and build include roots are required.
    set(MC2_DAWN_INCLUDES
        "${MC2_DAWN_DIR}/include"
        "${MC2_DAWN_BUILD}/gen/include"
    )

    find_library(MC2_DAWN_LIB
        NAMES webgpu_dawn
        PATHS "${MC2_DAWN_BUILD}" "${MC2_DAWN_BUILD}/src/dawn/native"
        PATH_SUFFIXES "" "Release"
        NO_DEFAULT_PATH)

    if(NOT MC2_DAWN_LIB)
        message(FATAL_ERROR "webgpu_dawn library not found under ${MC2_DAWN_BUILD}")
    endif()

    # webgpu_dawn.lib is a STATIC library, so Dawn's own PRIVATE platform
    # dependencies do not propagate to consumers and must be repeated here. This
    # list is not guesswork: it is exactly what Dawn's
    # src/dawn/native/CMakeLists.txt appends on WIN32 (user32, onecore_apiset)
    # plus dxguid for the D3D11/D3D12 backends. Omitting them fails the link with
    # unresolved WKPDID_D3DDebugObjectName and CompareObjectHandles.
    set(MC2_DAWN_SYSTEM_LIBS user32.lib onecore_apiset.lib dxguid.lib)

    # d3dcompiler_47.dll -- a RUNTIME requirement, and a subtle one.
    #
    # Dawn's D3D12 backend loads it through DynamicLib::Open, which passes
    # LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS. Those
    # flags reject a bare filename with ERROR_INVALID_PARAMETER (87), so the copy
    # in System32 is NOT found and device creation fails with a message about a
    # DLL that is plainly present on the machine. The DLL has to sit next to the
    # executable. Dawn's own comment says to ship the latest SDK compiler rather
    # than the OS one, so the SDK redist copy is preferred.
    find_file(MC2_D3DCOMPILER_DLL
        NAMES d3dcompiler_47.dll
        PATHS
            "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64"
            "C:/Program Files/Windows Kits/10/Redist/D3D/x64"
            "$ENV{WindowsSdkDir}Redist/D3D/x64"
        NO_DEFAULT_PATH)

    if(NOT MC2_D3DCOMPILER_DLL)
        message(WARNING
            "d3dcompiler_47.dll not found in the Windows SDK redist. The WebGPU "
            "backend will fail to create a device and fall back to the null "
            "renderer. Copy it next to the executable manually to fix this.")
    endif()

        set(MC2_DAWN_RENDERER_LIB "${MC2_DAWN_LIB}" ${MC2_DAWN_SYSTEM_LIBS})
        message(STATUS "MC2: WebGPU backend ON, linking ${MC2_DAWN_LIB}")
    endif()
endif()

if(EMSCRIPTEN)
    add_library(mc2_image_decode STATIC
        "${MC2_GAMEOS_DIR}/impl/renderer/emscripten/renderer_jpeg_libjpeg.cpp"
    )
    target_link_libraries(mc2_image_decode PUBLIC mc2_common)
    target_include_directories(mc2_image_decode PRIVATE
        "${MC2_GAMEOS_DIR}/impl/renderer"
    )
    target_compile_options(mc2_image_decode PRIVATE "SHELL:-sUSE_LIBJPEG=1")
    target_link_options(mc2_image_decode PUBLIC "SHELL:-sUSE_LIBJPEG=1")

    add_library(mc2_platform_emscripten STATIC
        "${MC2_GAMEOS_DIR}/impl/platform/emscripten/platform_emscripten.cpp"
        "${MC2_GAMEOS_DIR}/impl/platform/emscripten/heap_probe_emscripten.cpp"
    )
    target_link_libraries(mc2_platform_emscripten PUBLIC mc2_common)
    target_include_directories(mc2_platform_emscripten PRIVATE
        "${MC2_GAMEOS_DIR}/impl"
    )

    add_library(mc2_vfs_emscripten STATIC
        "${MC2_GAMEOS_DIR}/impl/vfs/emscripten/vfs_emscripten.cpp"
    )
    target_link_libraries(mc2_vfs_emscripten PUBLIC mc2_common)
    target_include_directories(mc2_vfs_emscripten PRIVATE
        "${MC2_GAMEOS_DIR}/impl/vfs"
    )
    # PORT (C2, owner ruling 2026-09-02): the browser audio peer. Third
    # implementation of the M6-4 seam; everything portable is already above it.
    add_library(mc2_audio_web STATIC
        "${MC2_GAMEOS_DIR}/impl/audio/web/audio_web.cpp"
    )
    target_link_libraries(mc2_audio_web PUBLIC mc2_common)
    target_include_directories(mc2_audio_web PRIVATE
        "${MC2_GAMEOS_DIR}/impl/audio"
    )

    set(MC2_PLATFORM_TARGET mc2_platform_emscripten)
    set(MC2_VFS_TARGET mc2_vfs_emscripten)
    set(MC2_AUDIO_TARGET mc2_audio_web)
elseif(WIN32)
    add_library(mc2_image_decode STATIC
        "${MC2_GAMEOS_DIR}/impl/renderer/win32/renderer_jpeg_wic.cpp"
    )
    target_link_libraries(mc2_image_decode PUBLIC mc2_common windowscodecs.lib ole32.lib)
    target_include_directories(mc2_image_decode PRIVATE
        "${MC2_GAMEOS_DIR}/impl/renderer"
    )

    add_library(mc2_platform_win32 STATIC
        "${MC2_GAMEOS_DIR}/impl/platform/win32/platform_win32.cpp"
        "${MC2_GAMEOS_DIR}/impl/platform/win32/crash_report_win32.cpp"
        "${MC2_GAMEOS_DIR}/impl/platform/win32/heap_probe_win32.cpp"
    )
    target_link_libraries(mc2_platform_win32 PUBLIC mc2_common user32.lib gdi32.lib dbghelp.lib)
    target_include_directories(mc2_platform_win32 PRIVATE
        "${MC2_GAMEOS_DIR}/impl"
    )

    add_library(mc2_vfs_native STATIC
        "${MC2_GAMEOS_DIR}/impl/vfs/native/vfs_native.cpp"
    )
    target_link_libraries(mc2_vfs_native PUBLIC mc2_common)
    target_include_directories(mc2_vfs_native PRIVATE
        "${MC2_GAMEOS_DIR}/impl/vfs"
    )
    # PORT: M6-4 native audio peer. waveOut only -- winmm is already linked for
    # the timer shims, and M6-4 explicitly rules out reimplementing DirectSound.
    add_library(mc2_audio_win32 STATIC
        "${MC2_GAMEOS_DIR}/impl/audio/win32/audio_win32.cpp"
    )
    target_link_libraries(mc2_audio_win32 PUBLIC mc2_common winmm)
    target_include_directories(mc2_audio_win32 PRIVATE
        "${MC2_GAMEOS_DIR}/impl"
    )

    set(MC2_PLATFORM_TARGET mc2_platform_win32)
    set(MC2_VFS_TARGET mc2_vfs_native)
    set(MC2_AUDIO_TARGET mc2_audio_win32)
else()
    message(FATAL_ERROR "No platform backend is selected for this host")
endif()

add_library(mc2_gameos STATIC ${MC2_GAMEOS_SOURCES})
target_link_libraries(mc2_gameos PUBLIC mc2_common mc2_image_decode)
target_include_directories(mc2_gameos PUBLIC "${MC2_GAMEOS_DIR}/impl")
target_link_libraries(mc2_gameos PUBLIC ${MC2_PLATFORM_TARGET} ${MC2_VFS_TARGET})
if(MC2_AUDIO_TARGET)
    target_link_libraries(mc2_gameos PUBLIC ${MC2_AUDIO_TARGET})
endif()

# PORT: M8-1. Import libraries only -- the DLLs are copied beside the exe, the
# same arrangement as d3dcompiler_47.dll.
if(MC2_ENABLE_FFMPEG)
    target_compile_definitions(mc2_gameos PRIVATE MC2_ENABLE_FFMPEG=1)
    target_include_directories(mc2_gameos PRIVATE "${MC2_FFMPEG_DIR}/include")
    target_link_libraries(mc2_gameos PUBLIC
        "${MC2_FFMPEG_DIR}/lib/avcodec.lib"
        "${MC2_FFMPEG_DIR}/lib/avformat.lib"
        "${MC2_FFMPEG_DIR}/lib/avutil.lib"
        "${MC2_FFMPEG_DIR}/lib/swscale.lib"
        "${MC2_FFMPEG_DIR}/lib/swresample.lib")

    # THESE ARE ORDINARY IMPORTS, BOUND BEFORE main(), and an attempt to make
    # them delay-loaded was measured and abandoned rather than left in looking
    # like it worked. /delayload: was passed to the linker in both the MSVC and
    # the lld spellings, with delayimp.lib linked; the produced exe carried the
    # five DLLs as normal dependencies and a DelayImport data-directory of size
    # 0 every time, both through this build and through a direct hand-written
    # lld-link 19.1.5 invocation on the same objects. So the DLL staging below
    # is not a convenience -- it is the ONLY thing keeping a missing DLL from
    # killing the process at load time. If a genuinely optional ffmpeg is ever
    # wanted, the route is explicit LoadLibrary/GetProcAddress binding in
    # video_ffmpeg.cpp, not a linker flag.
    #
    # Both the game and video_decode_test want these libraries the moment they
    # decode, so stage them in the common runtime directory as part of every
    # ffmpeg-enabled build: that is what stops a plain ctest run from raising a
    # blocking "DLL was not found" dialog, as it did on 2026-08-15.
    set(MC2_FFMPEG_RUNTIME_DLLS
        "${MC2_FFMPEG_DIR}/bin/avcodec-61.dll"
        "${MC2_FFMPEG_DIR}/bin/avformat-61.dll"
        "${MC2_FFMPEG_DIR}/bin/avutil-59.dll"
        "${MC2_FFMPEG_DIR}/bin/swscale-8.dll"
        "${MC2_FFMPEG_DIR}/bin/swresample-5.dll")
    foreach(_ffmpeg_dll IN LISTS MC2_FFMPEG_RUNTIME_DLLS)
        if(NOT EXISTS "${_ffmpeg_dll}")
            message(FATAL_ERROR "MC2_ENABLE_FFMPEG is ON but runtime DLL is missing: ${_ffmpeg_dll}")
        endif()
    endforeach()
    add_custom_target(mc2_ffmpeg_runtime ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${MC2_FFMPEG_RUNTIME_DLLS} "${CMAKE_BINARY_DIR}/bin"
        COMMENT "Copying ffmpeg runtime DLLs next to MC2 executables"
        VERBATIM)
endif()

if(MC2_ENABLE_WEBGPU)
    #-----------------------------------------------------------------------#
    # The WebGPU backend is its own target, for one hard reason: DAWN REQUIRES
    # C++20 and the rest of this project is C++14.
    #
    # Dawn's generated webgpu_cpp.h uses <span>, conditional explicit
    # (`explicit(false)`) and a `requires` clause, so it cannot be compiled at
    # C++14 at all -- and raising the whole project to C++20 would put 322
    # translation units of 2001-era code through five standards' worth of
    # removals (`register`, dynamic exception specifications, and friends) for no
    # benefit. Isolating it to one TU is both smaller and reversible.
    #
    # Mixing standards across TUs is safe HERE specifically because the seam is
    # RendererBackend: pure virtuals over GameOS POD types, declared in a header
    # that includes no standard containers. No std:: type crosses the boundary,
    # so there is no layout to disagree about.
    #
    # Dawn's include roots stay on THIS target only -- nothing else in the port
    # should be able to see a WebGPU header by accident.
    #-----------------------------------------------------------------------#
    if(EMSCRIPTEN)
        set(MC2_WEBGPU_RENDERER_TARGET mc2_renderer_emdawn)
    else()
        set(MC2_WEBGPU_RENDERER_TARGET mc2_renderer_webgpu)
    endif()
    add_library(${MC2_WEBGPU_RENDERER_TARGET} STATIC
        "${MC2_GAMEOS_DIR}/impl/renderer/webgpu/renderer_webgpu.cpp"
        "${MC2_GAMEOS_DIR}/impl/renderer/webgpu/renderer_webgpu_backend.cpp"
        "${MC2_GAMEOS_DIR}/impl/renderer/webgpu/renderer_webgpu_geometry.cpp"
        "${MC2_GAMEOS_DIR}/impl/renderer/webgpu/renderer_webgpu_pipelines.cpp"
        "${MC2_GAMEOS_DIR}/impl/renderer/webgpu/renderer_webgpu_surface.cpp"
        "${MC2_GAMEOS_DIR}/impl/renderer/webgpu/renderer_webgpu_targets.cpp"
        "${MC2_GAMEOS_DIR}/impl/renderer/webgpu/renderer_webgpu_text.cpp"
        "${MC2_GAMEOS_DIR}/impl/renderer/webgpu/renderer_webgpu_textures.cpp"
    )
    set_target_properties(${MC2_WEBGPU_RENDERER_TARGET} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    target_link_libraries(${MC2_WEBGPU_RENDERER_TARGET} PUBLIC mc2_common)
    target_include_directories(${MC2_WEBGPU_RENDERER_TARGET}
        PRIVATE "${MC2_GAMEOS_DIR}/impl" "${MC2_GAMEOS_DIR}/impl/renderer"
        PRIVATE ${MC2_DAWN_INCLUDES}
    )
    target_compile_definitions(${MC2_WEBGPU_RENDERER_TARGET} PRIVATE MC2_ENABLE_WEBGPU=1)
    target_link_libraries(${MC2_WEBGPU_RENDERER_TARGET} PUBLIC ${MC2_DAWN_RENDERER_LIB})

    # PUBLIC on mc2_gameos: renderer_select.cpp and renderer_backend.h both need
    # the macro to know the WebGPU backend exists. They do NOT need Dawn itself.
    target_compile_definitions(mc2_gameos PUBLIC MC2_ENABLE_WEBGPU=1)
    target_link_libraries(mc2_gameos PUBLIC ${MC2_WEBGPU_RENDERER_TARGET})
endif()

#---------------------------------------------------------------------------#
# GameOS unit tests.
#
# These exist because the game cannot link yet, so there is no other way to
# execute a GameOS subsystem. The memory manager in particular is already
# load-bearing -- MCLib's UserHeap routes every allocation through it -- and
# debugging an allocator for the first time inside a 322-TU binary during
# engine init is not a plan.
#---------------------------------------------------------------------------#
option(MC2_BUILD_TESTS "Build GameOS subsystem unit tests" ON)

if(MC2_BUILD_TESTS)
    add_executable(gos_memory_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/gos_memory_test.cpp")
    target_link_libraries(gos_memory_test PRIVATE mc2_gameos)
    set_target_properties(gos_memory_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    # PORT: M8-B4. A channel that has been emptied or reloaded must not still
    # report the previous sound's play mode -- MC2 reads exactly that value to
    # decide whether a channel is free, so a stale one loses the sound silently.
    # Backend-independent: it tests bookkeeping, not audible output.
    add_executable(audio_channel_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/audio_channel_test.cpp")
    target_link_libraries(audio_channel_test PRIVATE mc2_gameos)
    set_target_properties(audio_channel_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    # LZW codec round-trip. lzcomp.cpp/lzdecomp.cpp were transcribed by hand from
    # x86 assembly and must agree bit for bit -- they write and read .txm caches
    # and save games. lzcomp.cpp guards its GameOS includes behind _MBCS, which
    # lets both files build here without dragging in MCLib.
    add_executable(lz_roundtrip_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/lz_roundtrip_test.cpp"
        "${MC2_GAME_DIR}/MCLib/lzcomp.cpp"
        "${MC2_GAME_DIR}/MCLib/lzdecomp.cpp"
    )
    target_compile_definitions(lz_roundtrip_test PRIVATE _MBCS)
    set_target_properties(lz_roundtrip_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_executable(renderer_tga_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/renderer_tga_test.cpp"
    )
    target_link_libraries(renderer_tga_test PRIVATE mc2_common)
    target_include_directories(renderer_tga_test PRIVATE
        "${MC2_GAMEOS_DIR}/impl/renderer"
    )
    set_target_properties(renderer_tga_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    # PORT: M8-1. Drives the same Decoder interface MC2Movie drives. With no
    # ffmpeg and no converted movies it SKIPS and returns 0, so ctest stays
    # green on a tree that has neither -- the converted set is retail-derived
    # and never committed.
    add_executable(video_decode_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/video_decode_test.cpp"
        "${MC2_GAMEOS_DIR}/impl/video/video_null.cpp"
    )
    if(MC2_ENABLE_FFMPEG)
        target_sources(video_decode_test PRIVATE "${MC2_GAMEOS_DIR}/impl/video/video_ffmpeg.cpp")
        target_compile_definitions(video_decode_test PRIVATE MC2_ENABLE_FFMPEG=1)
        target_include_directories(video_decode_test PRIVATE "${MC2_FFMPEG_DIR}/include")
        target_link_libraries(video_decode_test PRIVATE
            "${MC2_FFMPEG_DIR}/lib/avcodec.lib"
            "${MC2_FFMPEG_DIR}/lib/avformat.lib"
            "${MC2_FFMPEG_DIR}/lib/avutil.lib"
            "${MC2_FFMPEG_DIR}/lib/swscale.lib"
            "${MC2_FFMPEG_DIR}/lib/swresample.lib")
        add_dependencies(video_decode_test mc2_ffmpeg_runtime)
    endif()
    # mc2_common carries _CRT_SECURE_NO_WARNINGS, as every other test target
    # gets it; without it this TU warns on getenv/fopen while the identical
    # code inside mc2_gameos does not.
    target_link_libraries(video_decode_test PRIVATE mc2_common)
    target_include_directories(video_decode_test PRIVATE "${MC2_GAMEOS_DIR}/impl/video")
    set_target_properties(video_decode_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    enable_testing()
    add_test(NAME gos_memory   COMMAND gos_memory_test)
    add_test(NAME audio_channel COMMAND audio_channel_test)
    add_test(NAME lz_roundtrip COMMAND lz_roundtrip_test)
    add_test(NAME renderer_tga COMMAND renderer_tga_test)
    add_test(NAME video_decode COMMAND video_decode_test)

    add_executable(renderer_font_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/renderer_font_test.cpp"
    )
    target_link_libraries(renderer_font_test PRIVATE mc2_common)
    target_include_directories(renderer_font_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gameos/impl/renderer"
    )
    set_target_properties(renderer_font_test PROPERTIES
        CXX_STANDARD 14
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(NAME renderer_font COMMAND renderer_font_test)

    add_executable(gos_input_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/gos_input_test.cpp"
    )
    target_link_libraries(gos_input_test PRIVATE mc2_gameos)
    target_include_directories(gos_input_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gameos/impl"
    )
    set_target_properties(gos_input_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(NAME gos_input COMMAND gos_input_test)
endif()

#---------------------------------------------------------------------------#
# mc2-crashwatch: runs the game under a debugger loop so a crash is described
# even when the in-process handlers do not run. __fastfail bypasses every
# user-mode handler by design, and on 2026-08-12 an ordinary unhandled access
# violation also produced no crash-report.txt. A debugger sees both.
#
# It links nothing from the engine on purpose: a diagnostic tool that shares
# code with the thing it is diagnosing is not a diagnostic tool.
#---------------------------------------------------------------------------#
if(WIN32 AND NOT EMSCRIPTEN)
    add_executable(mc2_crashwatch
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/crashwatch/mc2_crashwatch.cpp"
    )
    target_link_libraries(mc2_crashwatch PRIVATE dbghelp.lib psapi.lib)
    # This target links none of the engine, so it does not inherit mc2_common's
    # definitions; fopen is the right call here and the tree builds warning-free.
    target_compile_definitions(mc2_crashwatch PRIVATE _CRT_SECURE_NO_WARNINGS)
    set_target_properties(mc2_crashwatch PROPERTIES
        OUTPUT_NAME "mc2-crashwatch"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

#---------------------------------------------------------------------------#
# Engine libraries. Dependency order mirrors the original solution:
#   Stuff  <- MLR <- gosFX <- MCLib <- MechCmd2
#---------------------------------------------------------------------------#
add_library(mc2_stuff STATIC ${MC2_stuff_SOURCES})
target_link_libraries(mc2_stuff PUBLIC mc2_common mc2_gameos)

add_library(mc2_mlr STATIC ${MC2_mlr_SOURCES})
target_link_libraries(mc2_mlr PUBLIC mc2_common mc2_stuff)

add_library(mc2_gosfx STATIC ${MC2_gosfx_SOURCES})
target_link_libraries(mc2_gosfx PUBLIC mc2_common mc2_mlr)

add_library(mc2_mclib STATIC ${MC2_mclib_SOURCES})
target_link_libraries(mc2_mclib PUBLIC mc2_common mc2_gosfx)

#---------------------------------------------------------------------------#
# The game executable.
#
# Note: MechCmd2.vcproj compiles the GUI/*.cpp sources directly into the
# executable and does NOT link GUI.lib -- GUI.vcproj exists for the editor.
# The generated source list reflects the executable's real contents.
#
# Excluded from v1 per the port plan: Source/Editor, Source/Viewer, and
# DirectPlay multiplayer (the gos_Net* entry points are stubbed).
#---------------------------------------------------------------------------#
# PORT: M8-B7. The global operator new/delete replacement must be compiled INTO
# the executable. In a static library the linker only pulls it in if something
# already references it, so it would silently guard nothing.
add_executable(MechCmd2 ${MC2_mechcmd2_SOURCES}
    # PORT-ADDED sources. The generated list mirrors the retail vcproj and
    # says not to edit it by hand, so port files are appended here instead.
    "${MC2_GAMEOS_DIR}/impl/mem/page_guard_new.cpp"
    "${MC2_GAME_DIR}/Code/devtools.cpp")
if(MC2_ENABLE_FFMPEG)
    add_dependencies(MechCmd2 mc2_ffmpeg_runtime)
endif()
target_link_libraries(MechCmd2 PRIVATE mc2_common mc2_mclib)

# winmm supplies timeGetTime / timeSetEvent / timeKillEvent, which GameOS used
# for its multimedia-timer mouse polling. These are real Win32 APIs rather than
# GameOS functions, so they are linked rather than reimplemented. The Emscripten
# backend will need its own timer shim for these three.
if(WIN32)
    target_link_libraries(MechCmd2 PRIVATE winmm)
endif()

# zlib: compress2/uncompress, used by the fastfile reader, save games and packets.
target_link_libraries(MechCmd2 PRIVATE mc2_zlib)

if(EMSCRIPTEN)
    # PORT: M5-5 keeps the legacy synchronous VFS contract on WasmFS while
    # Asyncify bridges OPFS and emdawn's asynchronous adapter/device futures.
    target_link_options(MechCmd2 PRIVATE
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        # PORT: Keep the browser runtime alive after a bounded diagnostic run;
        # destroying WasmFS with legacy handles still open trips OPFS assertions.
        "SHELL:-sEXIT_RUNTIME=0"
        "SHELL:-sWASMFS=1"
        "SHELL:-sASYNCIFY=1"
        # PORT: MC2 has deep legacy combat/pathfinding/effect call chains. The
        # 64 KiB Emscripten defaults overflow during campaign load and weapon
        # volleys, presenting as a frozen browser main thread.
        "SHELL:-sSTACK_SIZE=4194304"
        "SHELL:-sASYNCIFY_STACK_SIZE=1048576"
        "SHELL:-lopfs.js"
        # PORT: C1. The link runs wasm-opt at -O2. This was blocked while the
        # link carried -sEMULATE_FUNCTION_POINTER_CASTS=1: directize met the
        # byn$fpcast-emu thunks and failed validation ("call* param number must
        # match"). That flag is gone -- with it removed, the owner's route
        # through mission, save, quickload, abort and EXIT raised no signature
        # trap. A real mismatch now traps loudly instead of being silently
        # redirected. With -g (the Debug tree the shell ships) binaryen runs
        # its DWARF-safe subset and says so in one link warning.
        -O2
    )
    set(MC2_EMSCRIPTEN_DATA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../mc2-run" CACHE PATH
        "Runtime tree from which the browser-safe FinalBuild subset is delivered")
    set(MC2_MC3_SHELL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../MechCommander3" CACHE PATH
        "MechCommander 3 static-shell checkout")
    set(MC2_MC3_PORT_DIR "${MC2_MC3_SHELL_DIR}/classic-mc2-port")
    if(NOT EXISTS "${MC2_MC3_SHELL_DIR}/server.mjs")
        message(FATAL_ERROR "MC3 static shell not found at ${MC2_MC3_SHELL_DIR}")
    endif()
    find_program(MC2_NODE_EXECUTABLE node REQUIRED)
    add_custom_target(mc2_browser_data ALL
        COMMAND "${MC2_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/package-browser-data.mjs"
                "${MC2_EMSCRIPTEN_DATA_ROOT}"
                "${MC2_MC3_PORT_DIR}/game-data"
        COMMENT "Delivering browser-safe MC2 data and OPFS manifest"
        VERBATIM)
    add_dependencies(MechCmd2 mc2_browser_data)
    set_target_properties(MechCmd2 PROPERTIES SUFFIX ".js")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/web/emscripten.html"
                   "${CMAKE_BINARY_DIR}/bin/index.html" COPYONLY)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/web/.nojekyll"
                   "${CMAKE_BINARY_DIR}/bin/.nojekyll" COPYONLY)
    add_custom_command(TARGET MechCmd2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MC2_MC3_PORT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:MechCmd2>" "${MC2_MC3_PORT_DIR}/MechCmd2.js"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE_DIR:MechCmd2>/MechCmd2.wasm" "${MC2_MC3_PORT_DIR}/MechCmd2.wasm"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/bin/index.html" "${MC2_MC3_PORT_DIR}/index.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/bin/.nojekyll" "${MC2_MC3_PORT_DIR}/.nojekyll"
        COMMENT "Deploying browser port to the MC3 static shell")

    # PORT: deploy the SHELL on its own schedule. The POST_BUILD rule above only
    # runs when MechCmd2 relinks, so editing web/emscripten.html alone left the
    # served index.html stale -- the change is made, the build reports success, and
    # the browser keeps running the old page. That has cost two debugging sessions.
    # This target depends on the HTML itself, so the copy happens whenever the
    # source changes and never when it has not.
    add_custom_command(
        OUTPUT "${MC2_MC3_PORT_DIR}/index.html"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MC2_MC3_PORT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/web/emscripten.html"
                "${MC2_MC3_PORT_DIR}/index.html"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/emscripten.html"
        COMMENT "Deploying browser shell (index.html)"
        VERBATIM)
    add_custom_target(mc2_browser_shell ALL
        DEPENDS "${MC2_MC3_PORT_DIR}/index.html")
endif()

set_target_properties(MechCmd2 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Put d3dcompiler_47.dll beside the executable. See the find_file above for why
# System32 does not satisfy Dawn's loader. This must travel with the binary: when
# MechCmd2.exe is copied into mc2-run\ for a test run, the DLL has to be copied
# too, or WebGPU silently falls back to the null renderer.
if(MC2_ENABLE_WEBGPU AND MC2_D3DCOMPILER_DLL)
    add_custom_command(TARGET MechCmd2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MC2_D3DCOMPILER_DLL}" "$<TARGET_FILE_DIR:MechCmd2>"
        COMMENT "Copying d3dcompiler_47.dll next to MechCmd2.exe (Dawn loads it from the exe dir)")
endif()

# Report EVERY undefined symbol, not the first 20. Until the GameOS backend is
# complete the link is expected to fail, and its full output is the point: it
# enumerates the exact entry points the game requires, which is ground truth to
# check against Phase 0's grep-derived estimate of ~95.
if(MSVC)
    target_link_options(MechCmd2 PRIVATE /errorlimit:0)
endif()

if(EMSCRIPTEN)
    message(STATUS "MC2 port: wasm32, ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
else()
    message(STATUS "MC2 port: 64-bit, ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()
message(STATUS "  stuff=${CMAKE_CURRENT_LIST_DIR}")
list(LENGTH MC2_stuff_SOURCES    _n_stuff)
list(LENGTH MC2_mlr_SOURCES      _n_mlr)
list(LENGTH MC2_gosfx_SOURCES    _n_gosfx)
list(LENGTH MC2_mclib_SOURCES    _n_mclib)
list(LENGTH MC2_mechcmd2_SOURCES _n_exe)
math(EXPR _n_total "${_n_stuff}+${_n_mlr}+${_n_gosfx}+${_n_mclib}+${_n_exe}")
message(STATUS "  sources: stuff=${_n_stuff} mlr=${_n_mlr} gosfx=${_n_gosfx} mclib=${_n_mclib} exe=${_n_exe} total=${_n_total}")
