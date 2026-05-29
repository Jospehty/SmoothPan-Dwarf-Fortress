# Add MinHook include directory
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/minhook/include)

# MinHook source files
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
    ${MINHOOK_SRCS}
)
