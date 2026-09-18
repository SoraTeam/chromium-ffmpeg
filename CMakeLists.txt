cmake_minimum_required(VERSION 3.30)
project(chromium_ffmpeg LANGUAGES C)

if(FFMPEG_HISTORICAL_TOOLCHAIN)
  # LLD 13 selects librarian mode only when /lib is its first argument.
  set(CMAKE_C_CREATE_STATIC_LIBRARY "<CMAKE_AR> /lib /nologo <LINK_FLAGS> /out:<TARGET> <OBJECTS>")
endif()

if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR
   NOT CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "X86" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Use Windows x86 with clang-cl and the Windows SDK (see WINDOWS.md).")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_GENERATOR STREQUAL "Ninja")
  message(FATAL_ERROR "Use the windows-x86-clang13 preset: optimized code is required by FFmpeg's configuration.")
endif()
set(FFMPEG_BRANDING "Chrome" CACHE STRING "Chromium configuration: Chrome or Chromium")
set_property(CACHE FFMPEG_BRANDING PROPERTY STRINGS Chrome Chromium)
if(NOT FFMPEG_BRANDING MATCHES "^(Chrome|Chromium)$")
  message(FATAL_ERROR "FFMPEG_BRANDING must be Chrome or Chromium")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_LINKER_TYPE LLD)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
# Immutable upstream archives. No system-wide installation or floating revisions.
FetchContent_Declare(gn
  URL https://chrome-infra-packages.appspot.com/dl/gn/gn/windows-amd64/+/5wPLBOHcmFQPLf_10R6kDW0yLN2GCkTUF8B2QyEHFyAC
  URL_HASH SHA256=e703cb04e1dc98540f2dfff5d11ea40d6d322cdd860a44d417c0764321071720
  DOWNLOAD_NAME gn.zip)
FetchContent_Declare(nasm
  URL https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip
  URL_HASH SHA256=3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6)
FetchContent_MakeAvailable(gn nasm)
set(CMAKE_ASM_NASM_COMPILER "${nasm_SOURCE_DIR}/nasm.exe")
set(CMAKE_ASM_NASM_OBJECT_FORMAT win32)
enable_language(ASM_NASM)

set(BUILD_SHARED_LIBS OFF)
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
  URL https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
  URL_HASH SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(opus)
target_compile_options(opus PRIVATE -flto=thin)

include(cmake/ChromiumSources.cmake)
set(platform_config "${PROJECT_SOURCE_DIR}/chromium/config/${FFMPEG_BRANDING}/win/ia32")

# The signature file is also Chromium's authoritative Windows DLL export list.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS chromium/ffmpeg.sigs)
file(STRINGS chromium/ffmpeg.sigs signatures)
set(exports "LIBRARY ffmpeg.dll\nEXPORTS\n")
set(export_checks "")
set(export_count 0)
foreach(signature IN LISTS signatures)
  string(STRIP "${signature}" signature)
  if(signature STREQUAL "" OR signature MATCHES "^//")
    continue()
  endif()
  if(NOT signature MATCHES "^[^()]+[ *]([A-Za-z_][A-Za-z_0-9]*)\\([^;]*\\);$")
    message(FATAL_ERROR "Unrecognized Chromium export signature: ${signature}")
  endif()
  string(APPEND exports "  ${CMAKE_MATCH_1}\n")
  string(APPEND export_checks "CHECK(GetProcAddress(module, \"${CMAKE_MATCH_1}\") != NULL);\n")
  math(EXPR export_count "${export_count} + 1")
endforeach()
string(APPEND export_checks "CHECK(export_directory->NumberOfNames == ${export_count});\n")
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/ffmpeg.def" CONTENT "${exports}" @ONLY)
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/export_checks.h" CONTENT "${export_checks}" @ONLY)

add_library(ffmpeg SHARED ${ffmpeg_c_sources} ${ffmpeg_asm_sources} "${PROJECT_BINARY_DIR}/ffmpeg.def")
target_link_libraries(ffmpeg PRIVATE opus)
target_include_directories(ffmpeg PRIVATE "${platform_config}" "${PROJECT_SOURCE_DIR}")
target_compile_definitions(ffmpeg PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:HAVE_AV_CONFIG_H;_POSIX_C_SOURCE=200112;_XOPEN_SOURCE=600;_ISOC99_SOURCE;_LARGEFILE_SOURCE;PIC;FFMPEG_CONFIGURATION=NULL;CHROMIUM_NO_LOGGING;_CRT_SECURE_NO_WARNINGS>")
target_compile_options(ffmpeg PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:/O2;/Z7;-flto=thin;/FIcompat/msvcrt/snprintf.h;/FIcompat/msvcrt/strtod.h;-Wno-deprecated-declarations;-Wno-incompatible-pointer-types;-Wno-inconsistent-dllimport;-Wno-constant-conversion>"
  "$<$<COMPILE_LANGUAGE:ASM_NASM>:-DARCH_X86_32;-DPREFIX;-DPIC;-P${platform_config}/config.asm;-I${PROJECT_SOURCE_DIR}/libavcodec/x86/;-I${PROJECT_SOURCE_DIR}/libavutil/x86/>")
target_compile_definitions(ffmpeg PRIVATE "$<$<COMPILE_LANGUAGE:C>:HAVE_EBP_AVAILABLE=0>")
# NASM includes do not all have compiler-generated depfiles on Windows.
file(GLOB asm_includes CONFIGURE_DEPENDS libavutil/x86/*.asm libavcodec/x86/*.asm)
set_source_files_properties(${ffmpeg_asm_sources} PROPERTIES OBJECT_DEPENDS "${asm_includes};${platform_config}/config.asm")
target_link_options(ffmpeg PRIVATE /OPT:REF /OPT:ICF /DEBUG /Brepro)

enable_testing()
add_executable(ffmpeg_smoke tests/windows_smoke.c)
target_include_directories(ffmpeg_smoke PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}" "${platform_config}")
target_link_libraries(ffmpeg_smoke PRIVATE ffmpeg)
target_compile_definitions(ffmpeg_smoke PRIVATE "EXPECT_H264=$<STREQUAL:${FFMPEG_BRANDING},Chrome>")
if(FFMPEG_HISTORICAL_TOOLCHAIN)
  target_compile_options(ffmpeg PRIVATE "$<$<COMPILE_LANGUAGE:C>:/guard:cf>")
  target_compile_options(opus PRIVATE /guard:cf)
  target_link_options(ffmpeg PRIVATE /guard:cf /LARGEADDRESSAWARE
    /SUBSYSTEM:CONSOLE,5.01 /OSVERSION:5.1 /PDBALTPATH:ffmpeg.dll.pdb)
  set_target_properties(ffmpeg PROPERTIES PDB_NAME ffmpeg.dll)
  target_compile_definitions(ffmpeg_smoke PRIVATE EXPECT_HISTORICAL_PE)
endif()
add_test(NAME dll-smoke COMMAND ffmpeg_smoke "$<TARGET_FILE:ffmpeg>")
add_test(NAME thinlto-bitcode COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
  "-DHISTORICAL_PACKAGE=${FFMPEG_HISTORICAL_TOOLCHAIN}" -P "${PROJECT_SOURCE_DIR}/cmake/CheckBitcode.cmake")

install(TARGETS ffmpeg RUNTIME DESTINATION . ARCHIVE DESTINATION .)
install(FILES "$<TARGET_PDB_FILE:ffmpeg>" DESTINATION .)
install(FILES COPYING.LGPLv2.1 LICENSE.md CREDITS.chromium WINDOWS.md DESTINATION licenses/ffmpeg)
install(FILES "${opus_SOURCE_DIR}/COPYING" DESTINATION licenses/opus)
install(FILES "${PROJECT_BINARY_DIR}/ffmpeg.def" DESTINATION .)
execute_process(COMMAND git describe --always --dirty
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE source_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT source_revision)
  set(source_revision "unknown (source archive)")
endif()
file(WRITE "${PROJECT_BINARY_DIR}/build-info.txt"
  "Source: ${source_revision}\nConfiguration: ${FFMPEG_BRANDING}/win/ia32\nCompiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\nCMake: ${CMAKE_VERSION}\nWindows SDK: $ENV{WindowsSDKVersion}\nCHROMIUM_NO_LOGGING=1\nLTO: ThinLTO (FFmpeg and Opus), LLD\nCRT: static\nOpus: 1.6.1\nNASM: 2.16.03\n")
install(FILES "${PROJECT_BINARY_DIR}/build-info.txt" DESTINATION .)
if(FFMPEG_HISTORICAL_TOOLCHAIN)
  file(APPEND "${PROJECT_BINARY_DIR}/build-info.txt"
    "Historical compiler package: ${FFMPEG_HISTORICAL_TOOLCHAIN}\nLLVM revision: fe5c2c3ca682b140dd5e640e75948363b6b25ef9\nMSVC headers/libs: $ENV{VCToolsVersion}\nSysroot provenance: Microsoft public VS 2019 components, not Google's 20d5f2553f archive\nPE OS/subsystem fields: 5.1 (not a Windows XP runtime guarantee)\n")
  foreach(sysroot_file IN ITEMS "$ENV{VCToolsInstallDir}/include/vcruntime.h"
      "$ENV{VCToolsInstallDir}/lib/x86/libcmt.lib"
      "$ENV{WindowsSdkDir}/Lib/10.0.19041.0/ucrt/x86/libucrt.lib"
      "$ENV{WindowsSdkDir}/Lib/10.0.19041.0/um/x86/kernel32.lib")
    file(SHA256 "${sysroot_file}" sysroot_hash)
    get_filename_component(sysroot_name "${sysroot_file}" NAME)
    file(APPEND "${PROJECT_BINARY_DIR}/build-info.txt" "Sysroot SHA256 ${sysroot_name}: ${sysroot_hash}\n")
  endforeach()
endif()
install(CODE "set(ffmpeg_pdb_name \"$<TARGET_PDB_FILE_NAME:ffmpeg>\")")
install(SCRIPT cmake/InstallChecksums.cmake)
