# SmoothPan — DFHack external plugin (Windows + Linux).
#
# Build inside a DFHack source tree: place (or symlink) this folder at
# dfhack/plugins/external/smoothpan and add `add_subdirectory(smoothpan)` to
# dfhack/plugins/external/CMakeLists.txt.  See tools/linux/build.sh and
# .github/workflows/release.yml for the scripted builds.

# SDL2 headers: DFHack's configure step downloads SDL2 headers and exposes
# them as SDL2_INCLUDE_DIRS.  A local SDL2/include (legacy Windows tree) or
# the system SDL2 are fallbacks.  SmoothPan never links SDL2: every SDL entry
# point is resolved at runtime from the SDL2 library DF already loaded.
if(SDL2_INCLUDE_DIRS)
    set(SMOOTHPAN_SDL2_INCLUDE ${SDL2_INCLUDE_DIRS})
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/SDL2/include/SDL.h)
    set(SMOOTHPAN_SDL2_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/SDL2/include)
else()
    find_path(SMOOTHPAN_SDL2_INCLUDE SDL.h PATH_SUFFIXES SDL2)
endif()
include_directories(${SMOOTHPAN_SDL2_INCLUDE})

set(SMOOTHPAN_SRCS
    smoothpan.cpp
    camera.cpp
    sdl_hook.cpp
    viewport.cpp
    renderer_hook.cpp
    debug_paths.cpp
    shift_mode.cpp
    mouse_comp.cpp
    probe.cpp
    trace.cpp
    perf.cpp
    designation_sync.cpp
    ffd_policy.cpp
    frame_seq.cpp
    zoom_probe.cpp
    compositor.cpp
    zoom_camera.cpp
    platform.cpp
    selftest.cpp
)

if(WIN32)
    # MinHook (inline hooks on SDL2.dll exports).  Vendored copy first, then
    # the legacy location next to the sources.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/minhook/src/hook.c)
        set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/minhook)
    else()
        set(MINHOOK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/minhook)
    endif()
    include_directories(${MINHOOK_DIR}/include)
    list(APPEND SMOOTHPAN_SRCS
        ${MINHOOK_DIR}/src/buffer.c
        ${MINHOOK_DIR}/src/hook.c
        ${MINHOOK_DIR}/src/trampoline.c
        ${MINHOOK_DIR}/src/hde/hde32.c
        ${MINHOOK_DIR}/src/hde/hde64.c
    )
    # DFHack builds with /WX; keep warnings visible in the log but non-fatal.
    dfhack_plugin(smoothpan ${SMOOTHPAN_SRCS} COMPILE_FLAGS_MSVC "/WX-")
else()
    # Linux: hooks are installed by rewriting the GOT slots of DF's modules
    # (see platform.cpp).  Needs libdl for dladdr/dlsym/dl_iterate_phdr.
    # DFHack builds with -Wall -Werror; keep warnings visible but never let a
    # newer local compiler's new warning break a user build (tools/linux/build.sh).
    dfhack_plugin(smoothpan ${SMOOTHPAN_SRCS} LINK_LIBRARIES dl COMPILE_FLAGS_GCC "-Wno-error")
endif()
