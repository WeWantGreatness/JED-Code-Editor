Debian package build
====================

This repository includes a minimal `debian/` packaging skeleton to build a `.deb` using `dpkg-buildpackage`.

Requirements (on Debian/Ubuntu):

- devscripts
- debhelper
- build-essential
- fakeroot

To build locally (if you have the build deps installed):

    dpkg-buildpackage -b -us -uc

Or use the `make package` target (which calls `dpkg-buildpackage`):

    make package

On CI (GitHub Actions) we install the build dependencies and run `dpkg-buildpackage -b -us -uc`, then upload the resulting `.deb` artifact.

Notes:
- The package is intentionally minimal; when you're ready we can improve packaging metadata, add lintian checks, and publish releases automatically.
