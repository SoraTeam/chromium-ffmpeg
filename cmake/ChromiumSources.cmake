# Let GN evaluate its own language. The upstream source list is copied verbatim;
# only its two Chromium-environment imports are supplied by this tiny host.
set(gn_root "${PROJECT_BINARY_DIR}/gn-sources")
file(MAKE_DIRECTORY "${gn_root}/build/config")
configure_file("${PROJECT_SOURCE_DIR}/ffmpeg_generated.gni" "${gn_root}/ffmpeg_generated.gni" COPYONLY)
file(WRITE "${gn_root}/.gn" "buildconfig = \"//config.gni\"\n")
file(WRITE "${gn_root}/config.gni" [=[
set_default_toolchain("//:unused")
is_android = false
is_linux = false
is_chromeos = false
is_fuchsia = false
is_mac = false
is_win = true
current_cpu = "x86"
]=])
file(WRITE "${gn_root}/build/config/arm.gni" "arm_use_neon = false\n")
file(WRITE "${gn_root}/ffmpeg_options.gni" "ffmpeg_branding = \"${FFMPEG_BRANDING}\"\n")
file(WRITE "${gn_root}/BUILD.gn" [=[
import("ffmpeg_generated.gni")
assert(ffmpeg_gas_sources == [], "Windows x86 should use NASM, not GAS")
write_file("$root_build_dir/sources.json", {
  c = ffmpeg_c_sources
  asm = ffmpeg_asm_sources
}, "json")
toolchain("unused") {
}
group("sources") {
}
]=])
execute_process(COMMAND "${gn_SOURCE_DIR}/gn.exe" gen "${gn_root}/out" "--root=${gn_root}"
  COMMAND_ERROR_IS_FATAL ANY)
file(READ "${gn_root}/out/sources.json" source_json)
foreach(kind IN ITEMS c asm)
  string(JSON count LENGTH "${source_json}" "${kind}")
  math(EXPR last "${count} - 1")
  set(ffmpeg_${kind}_sources "")
  foreach(index RANGE ${last})
    string(JSON path GET "${source_json}" "${kind}" ${index})
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${path}")
      message(FATAL_ERROR "Chromium source list names a missing file: ${path}")
    endif()
    list(APPEND ffmpeg_${kind}_sources "${PROJECT_SOURCE_DIR}/${path}")
  endforeach()
  message(STATUS "Chromium ${FFMPEG_BRANDING}/win/ia32: ${count} ${kind} sources")
endforeach()
