# CPCSyntaxError — an Amstrad CPC / CPC Plus / GX4000 emulator.
#
# Targets:
#   cpcse-core   the emulation core (static library, no host dependencies)
#   imgui        Dear ImGui, docking branch (third_party/imgui)
#   rasm         the RASM assembler, built in (third_party/rasm)
#   cpcse        the desktop emulator (SDL2 + OpenGL + Dear ImGui)
#
# SDL2 is not bundled. Point SDL2_ROOT at an SDL2 MinGW development package
# (the folder holding include/, lib/ and bin/) -- see third_party/SDL2/README.md.

cmake_minimum_required(VERSION 3.16)
project(CPCSyntaxError C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
                      $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-parameter>)
  set(CMAKE_CXX_FLAGS_RELEASE "-O2")
  # The emulator is full of deliberate integer wrap (8- and 16-bit counters that
  # MUST wrap exactly as the hardware does), so signed-overflow UB assumptions
  # are not wanted anywhere near it.
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fwrapv>)
endif()

# Link-time optimisation in Release: the per-pixel monitor path crosses several source
# files, and only the linker can inline across them (about 10% faster, same output).
# -DCPCSE_LTO=OFF to build without.
option(CPCSE_LTO "Link-time optimisation in Release builds" ON)
if(CPCSE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT cpcseIpo OUTPUT cpcseIpoWhy LANGUAGES CXX)
  if(cpcseIpo)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(STATUS "LTO not available: ${cpcseIpoWhy}")
  endif()
endif()

# --------------------------------------------------------------- the core
file(GLOB CPCSE_CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(cpcse-core STATIC ${CPCSE_CORE_SOURCES})
target_include_directories(cpcse-core PUBLIC src src/core)

# --------------------------------------------------------------- SDL2
set(SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2" CACHE PATH
    "SDL2 MinGW development package (the folder with include/, lib/, bin/)")
if(NOT EXISTS "${SDL2_ROOT}/include/SDL2/SDL.h")
  message(FATAL_ERROR "SDL2 not found in ${SDL2_ROOT}. Download the SDL2 MinGW development "
                      "package and pass -DSDL2_ROOT=<its x86_64-w64-mingw32 folder>, or unpack "
                      "it into third_party/SDL2 (see third_party/SDL2/README.md).")
endif()

# --------------------------------------------------------------- Dear ImGui
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends ${SDL2_ROOT}/include/SDL2)

# --------------------------------------------------------------- RASM
# RASM (Edouard BERGE, MIT) with every cruncher it ships. Each cruncher keeps its
# own include path: apultra, LZSA and salvador all have a matchfinder.h. exit()
# is renamed so a fatal assembly returns to the emulator (src/gui/rasm_bridge.c).
set(RASM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rasm)
set(RASM_DSS ${RASM_DIR}/apultra-master/src/libdivsufsort)
add_library(rasm-crunchers OBJECT
  ${RASM_DSS}/lib/divsufsort.c ${RASM_DSS}/lib/divsufsort_utils.c
  ${RASM_DSS}/lib/sssort.c ${RASM_DSS}/lib/trsort.c)
target_include_directories(rasm-crunchers PRIVATE ${RASM_DSS}/include)
add_library(rasm-apultra OBJECT
  ${RASM_DIR}/apultra-master/src/expand.c ${RASM_DIR}/apultra-master/src/matchfinder.c
  ${RASM_DIR}/apultra-master/src/shrink.c)
target_include_directories(rasm-apultra PRIVATE ${RASM_DSS}/include ${RASM_DIR}/apultra-master/src)
set(LZSA_SRC ${RASM_DIR}/lzsa-master/src)
add_library(rasm-lzsa OBJECT
  ${LZSA_SRC}/dictionary.c ${LZSA_SRC}/expand_block_v1.c ${LZSA_SRC}/expand_block_v2.c
  ${LZSA_SRC}/expand_context.c ${LZSA_SRC}/expand_inmem.c ${LZSA_SRC}/frame.c
  ${LZSA_SRC}/matchfinder.c ${LZSA_SRC}/shrink_block_v1.c ${LZSA_SRC}/shrink_block_v2.c
  ${LZSA_SRC}/shrink_context.c ${LZSA_SRC}/shrink_inmem.c ${LZSA_SRC}/stream.c)
target_include_directories(rasm-lzsa PRIVATE ${RASM_DSS}/include ${LZSA_SRC})
add_library(rasm-salvador OBJECT
  ${RASM_DIR}/salvador/src/expand.c ${RASM_DIR}/salvador/src/matchfinder.c
  ${RASM_DIR}/salvador/src/shrink.c)
target_include_directories(rasm-salvador PRIVATE ${RASM_DSS}/include ${RASM_DIR}/salvador/src)
add_library(rasm STATIC
  ${RASM_DIR}/rasm.c
  src/gui/rasm_bridge.c
  $<TARGET_OBJECTS:rasm-crunchers> $<TARGET_OBJECTS:rasm-apultra>
  $<TARGET_OBJECTS:rasm-lzsa> $<TARGET_OBJECTS:rasm-salvador>)
target_include_directories(rasm PUBLIC ${RASM_DIR})
set_source_files_properties(${RASM_DIR}/rasm.c PROPERTIES
  COMPILE_DEFINITIONS "INTEGRATED_ASSEMBLY;RASM_PRODUCTION_WITHOUT_AUTOTEST;exit=rasm_bridge_exit")
# ...and without link-time optimisation: rasm.c declares salvador_compress and
# salvador_get_max_compressed_size with other types than salvador's sources define them.
foreach(t rasm rasm-crunchers rasm-apultra rasm-lzsa rasm-salvador)
  target_compile_options(${t} PRIVATE -w -O2)
  set_target_properties(${t} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
endforeach()
if(WIN32)
  target_link_libraries(rasm PUBLIC ws2_32)   # rasm's web directives
endif()

# --------------------------------------------------------------- the emulator
add_executable(cpcse
  src/gui/main.cpp
  src/gui/emuhost.cpp
  src/gui/gui_shell.cpp
  src/gui/gui_widgets.cpp
  src/gui/gui_debugger.cpp
  src/gui/gui_debug_views.cpp
  src/gui/gui_assembler.cpp
)
# A GUI-subsystem executable: no console window. SDL2main provides the WinMain
# and forwards to main() (renamed SDL_main).
set_target_properties(cpcse PROPERTIES WIN32_EXECUTABLE TRUE)
target_include_directories(cpcse PRIVATE src ${SDL2_ROOT}/include/SDL2)
# Link order matters on MinGW: mingw32, SDL2main, then the SDL2 DLL import library.
target_link_libraries(cpcse PRIVATE
  cpcse-core imgui rasm
  mingw32
  ${SDL2_ROOT}/lib/libSDL2main.a
  ${SDL2_ROOT}/lib/libSDL2.dll.a
  opengl32 gdi32 winmm imm32)
if(MINGW)
  # Ship one DLL (SDL2.dll), not the GCC runtime as well.
  target_link_options(cpcse PRIVATE -static-libgcc -static-libstdc++ -static)
endif()
# SDL2.dll must sit next to the executable at runtime, and the firmware in roms/
# beside it is what the emulator boots from.
add_custom_command(TARGET cpcse POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL2_ROOT}/bin/SDL2.dll $<TARGET_FILE_DIR:cpcse>
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/roms $<TARGET_FILE_DIR:cpcse>/roms)
