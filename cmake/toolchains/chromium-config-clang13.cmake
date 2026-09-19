# Reproduce the compiler revision recorded in chromium/config/*/win/ia32.
# This is NOT the later compiler revision pinned by Electron 13's Chromium DEPS.
# Run in a VS 2019 environment initialized with:
# Common7/Tools/VsDevCmd.bat -arch=x86 -host_arch=x64
#                          -vcvars_ver=14.26.28801 -winsdk=10.0.19041.0
set(_revision fe5c2c3ca682b140dd5e640e75948363b6b25ef9)
set(_package llvmorg-13-init-3462-gfe5c2c3c-2)
set(_archive_hash 62f5cc63e7bb6e4fac69dc9f0414e0ae79269add462c2044f854596b2899ce25)

foreach(_version IN ITEMS VCToolsVersion WindowsSDKVersion)
  string(REGEX REPLACE "[/\\\\]+$" "" _${_version} "$ENV{${_version}}")
endforeach()
if(NOT _VCToolsVersion STREQUAL "14.26.28801" OR
   NOT _WindowsSDKVersion STREQUAL "10.0.19041.0")
  message(FATAL_ERROR "Historical profile requires MSVC 14.26.28801 headers/libs and SDK 10.0.19041.0; got '$ENV{VCToolsVersion}' / '$ENV{WindowsSDKVersion}'. See WINDOWS.md. No toolchain fallback is allowed.")
endif()
file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" _msvc)
file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" _sdk)
foreach(_file IN ITEMS
    "${_msvc}/include/vcruntime.h" "${_msvc}/lib/x86/libcmt.lib"
    "${_sdk}/Include/10.0.19041.0/um/Windows.h"
    "${_sdk}/Include/10.0.19041.0/ucrt/stdio.h"
    "${_sdk}/Lib/10.0.19041.0/um/x86/kernel32.lib"
    "${_sdk}/Lib/10.0.19041.0/ucrt/x86/libucrt.lib"
    "${_sdk}/bin/10.0.19041.0/x64/rc.exe"
    "${_sdk}/bin/10.0.19041.0/x64/mt.exe")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "Incomplete historical sysroot: ${_file}")
  endif()
endforeach()
# Avoid accidentally mixing in a newer SDK, CRT or the host architecture.
set(ENV{INCLUDE} "${_msvc}/include;${_sdk}/Include/10.0.19041.0/ucrt;${_sdk}/Include/10.0.19041.0/shared;${_sdk}/Include/10.0.19041.0/um;${_sdk}/Include/10.0.19041.0/winrt")
set(ENV{LIB} "${_msvc}/lib/x86;${_sdk}/Lib/10.0.19041.0/ucrt/x86;${_sdk}/Lib/10.0.19041.0/um/x86")

get_filename_component(_cache "${CMAKE_CURRENT_LIST_DIR}/../../out/toolchains" ABSOLUTE)
set(_root "${_cache}/${_package}")
if(NOT EXISTS "${_root}/bin/clang-cl.exe")
  file(MAKE_DIRECTORY "${_cache}")
  file(DOWNLOAD
    "https://commondatastorage.googleapis.com/chromium-browser-clang/Win/clang-${_package}.tgz"
    "${_cache}/clang-${_package}.tgz"
    EXPECTED_HASH "SHA256=${_archive_hash}" TLS_VERIFY ON)
  file(ARCHIVE_EXTRACT INPUT "${_cache}/clang-${_package}.tgz" DESTINATION "${_root}")
endif()
foreach(_tool IN ITEMS clang-cl lld-link)
  execute_process(COMMAND "${_root}/bin/${_tool}.exe" --version
    OUTPUT_VARIABLE _version_text COMMAND_ERROR_IS_FATAL ANY)
  if(NOT _version_text MATCHES "${_revision}")
    message(FATAL_ERROR "${_tool} is not the pinned Chromium revision: ${_version_text}")
  endif()
endforeach()
set(CMAKE_C_COMPILER "${_root}/bin/clang-cl.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_root}/bin/clang-cl.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_TARGET i686-pc-windows-msvc CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER_TARGET i686-pc-windows-msvc CACHE STRING "" FORCE)
# Persist the sysroot in Ninja's commands. Environment changes in the configure
# process do not survive the workflow's separate build process.
set(CMAKE_C_FLAGS_INIT /X)
set(CMAKE_CXX_FLAGS_INIT /X)
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES
  "${_root}/lib/clang/13.0.0/include"
  "${_msvc}/include"
  "${_sdk}/Include/10.0.19041.0/ucrt"
  "${_sdk}/Include/10.0.19041.0/shared"
  "${_sdk}/Include/10.0.19041.0/um"
  "${_sdk}/Include/10.0.19041.0/winrt")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_C_STANDARD_INCLUDE_DIRECTORIES})
set(_libpaths "\"/libpath:${_msvc}/lib/x86\" \"/libpath:${_sdk}/Lib/10.0.19041.0/ucrt/x86\" \"/libpath:${_sdk}/Lib/10.0.19041.0/um/x86\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_libpaths}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_libpaths}")
set(CMAKE_LINKER "${_root}/bin/lld-link.exe" CACHE FILEPATH "" FORCE)
# Chromium's Windows archive ships no llvm-lib; LLD's native librarian mode
# accepts LLVM bitcode without requiring an unrelated LLVM/MSVC librarian.
set(CMAKE_AR "${_root}/bin/lld-link.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER "${_sdk}/bin/10.0.19041.0/x64/rc.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_MT "${_sdk}/bin/10.0.19041.0/x64/mt.exe" CACHE FILEPATH "" FORCE)
set(FFMPEG_HISTORICAL_TOOLCHAIN "${_package}" CACHE INTERNAL "Pinned config-generation compiler")
