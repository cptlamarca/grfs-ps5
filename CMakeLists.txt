# Ghost Recon: Future Soldier - native PS5 port, on the ReXGlue runtime's PS5
# platform layer. Build:
#   cmake -S . -B out/ps5 -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/ps5-toolchain.cmake \
#         -DPS5_PAYLOAD_SDK=<ps5-payload-sdk> -DREXSDK_DIR=<rexglue-sdk, ps5 branch>
# Code: generated/ comes from 'rexglue codegen grfs_manifest.toml' on the host
# (from the disc's default.xex, never in this repository), then
# tools/ps5_patch_generated.py.
cmake_minimum_required(VERSION 3.25)

project(grfs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The PS5: no profiler, and none of SDL's desktop backends (the runtime's PS5
# platform layer has the display, audio and the DualSense).
if(REX_PS5)
    set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)
    foreach(opt SDL_DBUS SDL_HIDAPI SDL_IBUS SDL_JACK SDL_KMSDRM SDL_LIBUDEV SDL_OFFSCREEN SDL_OSS SDL_SNDIO)
        set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
endif()

include(generated/rexglue.cmake)

add_executable(grfs src/main.cpp src/grfs_app.cpp src/achievements_ui.cpp src/achievements_menu.cpp
    src/progression_hooks.cpp
    src/news_hooks.cpp
    src/connection_menu.cpp
    src/support_screen.cpp
    src/player_account.cpp
    src/language_patch.cpp
    src/shader_seed.cpp
    src/dualsense.cpp
    src/frame_rate.cpp
    src/disc_image.cpp
    src/iso_screen.cpp
    src/third_party/qrcodegen/qrcodegen.cpp
    src/trophy_sound.cpp)
rexglue_setup_target(grfs GPU_PLUGINS xenos)
if(REXSDK_DIR AND TARGET imgui)
    # SDK source tree: ImGui (built into the runtime) and xxHash (rex/hash.h)
    # are not on the public include path the way the installed SDK bundles them.
    target_include_directories(grfs PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
                                            ${REXSDK_DIR}/thirdparty/stb
                                            ${REXSDK_DIR}/thirdparty/xxHash)
endif()

# The AI (AICLASS_X360_F.xex, a guest DLL the game loads and reloads): on the
# PS5 the app can't dlopen its own libraries, so the module's recompiled code
# is linked in. Its objects are partially linked into one, where only its two
# entry points stay global (renamed), so the helpers both modules define under
# the same names don't clash; src/ai_module.cpp hands them to the runtime.
include(generated/aiclass_x360_f/sources.cmake)
set(GRFS_AI_SOURCES ${GENERATED_SOURCES})
unset(GENERATED_SOURCES)
# src/ai_hooks.cpp patches the module's game logic.
add_library(grfs_ai OBJECT ${GRFS_AI_SOURCES} src/ai_hooks.cpp)
target_include_directories(grfs_ai PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/generated/aiclass_x360_f)
target_link_libraries(grfs_ai PRIVATE rex::runtime)
target_compile_definitions(grfs_ai PRIVATE
    ReXModule_Register=grfs_ai_ReXModule_Register
    ReXModule_GetImageInfo=grfs_ai_ReXModule_GetImageInfo)
rexglue_apply_target_settings(grfs_ai)
rexglue_apply_recomp_settings(grfs_ai "${CMAKE_CURRENT_SOURCE_DIR}/generated/aiclass_x360_f/grfs_pch.h")

find_program(GRFS_LD_LLD NAMES ld.lld REQUIRED)
find_program(GRFS_LLVM_OBJCOPY NAMES llvm-objcopy REQUIRED)
set(GRFS_AI_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/grfs_ai_module.o")
add_custom_command(
    OUTPUT "${GRFS_AI_OBJECT}"
    COMMAND "${GRFS_LD_LLD}" -r -o "${GRFS_AI_OBJECT}.partial"
            "$<FILTER:$<TARGET_OBJECTS:grfs_ai>,INCLUDE,[.]o$>"
    COMMAND "${GRFS_LLVM_OBJCOPY}" --keep-global-symbol=grfs_ai_ReXModule_Register
            --keep-global-symbol=grfs_ai_ReXModule_GetImageInfo
            "${GRFS_AI_OBJECT}.partial" "${GRFS_AI_OBJECT}"
    DEPENDS grfs_ai $<TARGET_OBJECTS:grfs_ai>
    COMMAND_EXPAND_LISTS
    COMMENT "Linking the AI module into one object"
    VERBATIM)
add_custom_target(grfs_ai_module DEPENDS "${GRFS_AI_OBJECT}")
add_dependencies(grfs grfs_ai_module)
target_sources(grfs PRIVATE src/ai_module.cpp)
target_link_libraries(grfs PRIVATE "${GRFS_AI_OBJECT}")
