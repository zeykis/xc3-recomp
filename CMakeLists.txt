cmake_minimum_required(VERSION 3.20)
project(xc3recomp C CXX)

# Requires clang (Linux) or clang-cl (Windows, "LLVM" toolset in Visual Studio):
# the runtime/generated code use GNU extensions (__int128, statement expressions, __builtin_*).
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB GEN_SOURCES ${CMAKE_SOURCE_DIR}/gen/main_*.c)
# Without generated code (no game\main.elf yet) only the tools are built: xc3-setup (installs the game files from your
# own dump), xc3-launcher, xc3-shaderc. Then: python recomp/recompile.py and build again (build-windows.bat does both).
if(GEN_SOURCES)
  set(XC3_HAVE_GEN ON)
else()
  set(XC3_HAVE_GEN OFF)
  message(STATUS "No generated code (gen/): building the tools only. Run recomp/recompile.py (needs game/main.elf) for xc3.")
endif()

if(XC3_HAVE_GEN)
add_library(xc3gen STATIC ${GEN_SOURCES} ${CMAKE_SOURCE_DIR}/gen/functable.c)
target_include_directories(xc3gen PRIVATE runtime/include gen)
# Generated code (the recompiled game) is optimized at XC3_GEN_OPT (default -O2: M5, the game's own code is on the
# critical path; -O1 builds faster). No debug info for it: the crash handler names functions from the function table,
# and -g on ~430 MB of C costs a lot of compile and link time.
set(XC3_GEN_OPT "-O2" CACHE STRING "optimization level for the generated game code")
target_compile_options(xc3gen PRIVATE ${XC3_GEN_OPT} -g0 -w -fno-strict-aliasing)

add_executable(xc3
  runtime/src/main.cpp
  runtime/src/loader.c
  runtime/src/hle.c
  runtime/src/hle_nn.cpp
  runtime/src/hle_cxx.cpp
  runtime/src/hle_fiber.cpp
  runtime/src/hle_audio.cpp
  runtime/src/audio_host.cpp
  runtime/src/hle_nvn.cpp
  runtime/src/findctr.cpp
  runtime/src/gpu_vk.cpp
  runtime/src/nvn_format.cpp
  runtime/src/input.cpp
  runtime/src/hle_movie.cpp
  runtime/src/hle_image.cpp
  runtime/src/platform.cpp
  runtime/src/sampler.cpp
  runtime/src/settings.cpp
  runtime/src/ui.cpp
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(xc3 PRIVATE runtime/include runtime/src third_party/vulkan/include third_party third_party/imgui)
# Dear ImGui 1.91.9 (MIT, third_party/imgui): the TAB settings overlay. Its Vulkan backend loads its functions through
# the runtime (gpu_present.inc), so its command recording goes through the recorder thread like ours.
target_compile_definitions(xc3 PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(xc3 PRIVATE xc3gen)

# ---------------------------------------------------------------- audio (M4)
# libopus 1.5.2 (BSD-3-Clause, third_party/opus) decodes the game's Opus streams (nn::codec::HardwareOpusDecoder);
# miniaudio 0.11.21 (public domain / MIT-0, third_party/miniaudio) plays the output. Neither needs an SDK.
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)
target_link_libraries(xc3 PRIVATE opus)

# ---------------------------------------------------------------- video (M4)
# FFmpeg (LGPL, shared) demuxes and decodes the cutscenes (WebM / VP9) behind the game's movie:: API.
# Windows: a pinned BtbN build in third_party/ffmpeg (fetched by build-windows.bat, not committed); its DLLs are
# copied next to xc3.exe. Linux: the system's libavcodec / libavformat / libavutil (pkg-config).
if(WIN32)
  set(FFMPEG_DIR ${CMAKE_SOURCE_DIR}/third_party/ffmpeg)
  if(NOT EXISTS ${FFMPEG_DIR}/lib/avcodec.lib)
    message(FATAL_ERROR "FFmpeg missing in third_party/ffmpeg - run build-windows.bat (it downloads it)")
  endif()
  target_include_directories(xc3 PRIVATE ${FFMPEG_DIR}/include)
  target_link_libraries(xc3 PRIVATE ${FFMPEG_DIR}/lib/avformat.lib ${FFMPEG_DIR}/lib/avcodec.lib ${FFMPEG_DIR}/lib/avutil.lib)
  file(GLOB FFMPEG_DLLS ${FFMPEG_DIR}/bin/*.dll)
  add_custom_command(TARGET xc3 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:xc3>)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
  target_link_libraries(xc3 PRIVATE PkgConfig::FFMPEG)
endif()
# Vulkan is loaded at runtime (vulkan-1.dll / libvulkan.so.1): no SDK or import library needed
if(WIN32)
  target_link_libraries(xc3 PRIVATE user32)
else()
  target_link_libraries(xc3 PRIVATE m pthread dl)
endif()

endif()   # XC3_HAVE_GEN

# ---------------------------------------------------------------- shader translator (M3.5)
# Maxwell -> SPIR-V translator from the Eden emulator (GPL-3.0-or-later), with its SPIR-V emitter
# (sirit), SPIR-V headers, and the Boost/fmt headers it needs, all vendored in third_party/.
file(GLOB_RECURSE XC3_SHADER_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/third_party/shader_recompiler/*.cpp
  ${CMAKE_SOURCE_DIR}/third_party/sirit/src/*.cpp)
add_library(xc3shader STATIC ${XC3_SHADER_SOURCES} third_party/eden_common/shim.cpp)
target_compile_features(xc3shader PUBLIC cxx_std_20)
target_include_directories(xc3shader
  PUBLIC third_party/boost/include third_party/fmt/include third_party/eden_common third_party
         third_party/sirit/include third_party/spirv-headers/include
  PRIVATE third_party/sirit/src)
target_compile_definitions(xc3shader PUBLIC FMT_HEADER_ONLY BOOST_ALL_NO_LIB)
target_compile_options(xc3shader PRIVATE -w)

# the runtime translates shaders at draw time
if(XC3_HAVE_GEN)
  target_sources(xc3 PRIVATE runtime/src/gpu_shader.cpp)
  target_link_libraries(xc3 PRIVATE xc3shader)
endif()

# xc3-shaderc: translate shaders captured with XRT_DUMP_SHADERS=1 and report coverage
add_executable(xc3-shaderc tools/shaderc/shaderc.cpp)
target_link_libraries(xc3-shaderc PRIVATE xc3shader)

# ---------------------------------------------------------------- installer (tools/setup)
# xc3-setup.exe: installs the game from the player's own files (base game + update + DLC; .xci / .nsp / .nca or an
# emulator's installed contents). make_installer.bat packages it with xc3.exe, hactool and the patches.
if(WIN32)
  add_executable(xc3-setup WIN32 tools/setup/setup_win.cpp tools/setup/nx.cpp)
  target_include_directories(xc3-setup PRIVATE tools/setup gen)
  target_link_libraries(xc3-setup PRIVATE ole32 shell32 comctl32 uuid)
  # XC3 Launcher: Play / Play Randomized (own save folders, the randomizer's files) - tools/launcher
  add_executable(xc3-launcher WIN32 tools/launcher/launcher_win.cpp)
  target_link_libraries(xc3-launcher PRIVATE ole32 shell32 comctl32 uuid urlmon bcrypt)
endif()
