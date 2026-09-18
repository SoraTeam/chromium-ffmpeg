# Runs after installation, including cmake --install and the install target.
set(package_dir "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
set(checksums "")
foreach(name IN ITEMS build-info.txt ffmpeg.def ffmpeg.dll ffmpeg.lib "${ffmpeg_pdb_name}")
  file(SHA256 "${package_dir}/${name}" hash)
  string(APPEND checksums "${hash}  ${name}\n")
endforeach()
file(WRITE "${package_dir}/SHA256SUMS" "${checksums}")
