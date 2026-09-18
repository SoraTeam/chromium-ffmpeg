# CI provisioning only. Local developers select these components in VS Installer.
if(NOT CMAKE_HOST_WIN32 OR "$ENV{RUNNER_TEMP}" STREQUAL "")
  message(FATAL_ERROR "This installer is for an ephemeral Windows CI runner; see WINDOWS.md for local setup.")
endif()
set(root "$ENV{RUNNER_TEMP}/ffmpeg-vs2019")
file(MAKE_DIRECTORY "${root}")
file(DOWNLOAD
  https://download.visualstudio.microsoft.com/download/pr/e2324e87-3765-4b14-85e5-1234d99f6254/0a641c8f47df21f3569fbd87f1f2301ae82db9bc5669bb07b18fb623a207c9ba/vs_BuildTools.exe
  "${root}/vs_buildtools.exe"
  EXPECTED_HASH SHA256=0a641c8f47df21f3569fbd87f1f2301ae82db9bc5669bb07b18fb623a207c9ba TLS_VERIFY ON)
file(DOWNLOAD
  https://download.visualstudio.microsoft.com/download/pr/e2324e87-3765-4b14-85e5-1234d99f6254/67d1892e69945c592dd0409c0054d80e5ec8816b874c243616fc8d426fab9120/VisualStudio.16.Release.chman
  "${root}/channel.chman"
  EXPECTED_HASH SHA256=ce478cd78cce92c5c8bdcf4bd5cb17f90ec7be245a772a030563cc7aa096c4c6 TLS_VERIFY ON)
file(TO_NATIVE_PATH "${root}/BuildTools" install_path)
file(TO_NATIVE_PATH "${root}/channel.chman" channel_path)
execute_process(COMMAND "${root}/vs_buildtools.exe" --quiet --wait --norestart --nocache
  --installPath "${install_path}" --channelUri "${channel_path}"
  --add Microsoft.VisualStudio.Component.VC.14.26.x86.x64
  --add Microsoft.VisualStudio.Component.Windows10SDK.19041
  RESULT_VARIABLE result)
message(STATUS "VS bootstrapper result: ${result}; installer logs: $ENV{TEMP}/dd_*.log")
if(NOT result EQUAL 0 AND NOT result EQUAL 3010)
  message(FATAL_ERROR "VS 2019 component installation failed: ${result}. See runner TEMP/dd_* logs.")
endif()
# The side-by-side 14.26 component provides VsDevCmd and its VC extension, not
# the default toolset's VC/Auxiliary/Build/vcvarsall.bat wrapper.
foreach(required IN ITEMS
    Common7/Tools/VsDevCmd.bat
    Common7/Tools/vsdevcmd/ext/vcvars.bat
    VC/Tools/MSVC/14.26.28801/include/vcruntime.h
    VC/Tools/MSVC/14.26.28801/lib/x86/libcmt.lib)
  if(NOT EXISTS "${root}/BuildTools/${required}")
    message(FATAL_ERROR "VS bootstrapper returned ${result}, but ${required} is missing from ${install_path}. Inspect TEMP/dd_* logs before building.")
  endif()
endforeach()
