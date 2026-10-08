# Dependency overlays

`x264` mirrors the port from vcpkg baseline `9432c416e5c543eded0b4df35a4d347b2e669208`.
Only the source transport changes: fetch the original VideoLAN Git repository at
commit `31e19f92f00c7003fa115047ce50978bc98c3a0d`, instead of its generated GitLab archive.
The archive endpoint returned bytes that failed the pinned SHA-512 in Windows CI
run 37708715628. The exact source commit, build options and patches remain pinned.
Do not replace the expected digest with that of an error page or disable verification.

Upstream port files are MIT licensed. See `LICENSE.vcpkg`.
Remove the overlay once the pinned registry has a working source transport.
