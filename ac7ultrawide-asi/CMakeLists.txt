cmake_minimum_required(VERSION 3.20)
project(ac7ultrawide LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# ac7ultrawide.asi
# Win64 DLL loaded by Ultimate ASI Loader (version.dll) at game startup.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enforce 64-bit - must match Ace7Game.exe (x64)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "Must build for x64.\n"
        "Run:  cmake -B build -A x64\n"
        "or:   cmake -B build -G \"Visual Studio 17 2022\" -A x64")
endif()

# ---------------------------------------------------------------------------
# MinHook — lightweight x86/x64 inline hooking library (MIT)
# Used to detour ID3D11Device::CreatePixelShader for HUD shader replacement.
# ---------------------------------------------------------------------------
add_library(minhook STATIC
    external/minhook/src/buffer.c
    external/minhook/src/hook.c
    external/minhook/src/trampoline.c
    external/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC external/minhook/include)
if(MSVC)
    set_property(TARGET minhook PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(ac7ultrawide SHARED src/dllmain.cpp)

# Output as .asi, no 'lib' prefix
set_target_properties(ac7ultrawide PROPERTIES
    OUTPUT_NAME "ac7ultrawide"
    SUFFIX      ".asi"
    PREFIX      "")

target_compile_definitions(ac7ultrawide PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS)

target_link_libraries(ac7ultrawide PRIVATE minhook)

if(MSVC)
    # Optimised release build, treat warnings as info only
    target_compile_options(ac7ultrawide PRIVATE /W4 /WX- /O2)

    # Static CRT - avoids a MSVCRT dependency in the final .asi
    set_property(TARGET ac7ultrawide PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# Optional: copy the built .asi straight into the game root so you can
# test immediately without a manual file copy step.
# Set GAME_DIR on the cmake command line if needed, e.g.:
#   cmake -B build -A x64 -DGAME_DIR="F:/SteamLibrary/steamapps/common/ACE COMBAT 7"
# ---------------------------------------------------------------------------
if(DEFINED GAME_DIR)
    add_custom_command(TARGET ac7ultrawide POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:ac7ultrawide>
            "${GAME_DIR}/ac7ultrawide.asi"
        COMMENT "Copying ac7ultrawide.asi to game directory")
endif()
