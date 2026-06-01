# Add MinHook and SDL2 include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/minhook/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/SDL2/include)

set(MINHOOK_SRCS
    minhook/src/buffer.c
    minhook/src/hook.c
    minhook/src/trampoline.c
    minhook/src/hde/hde32.c
    minhook/src/hde/hde64.c
)

dfhack_plugin(smoothpan
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
    ${MINHOOK_SRCS}
)
