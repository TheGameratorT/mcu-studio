# AUR package

`PKGBUILD` and `.SRCINFO` for the [`mcu-studio`](https://aur.archlinux.org/packages/mcu-studio)
AUR package. This directory is the source of truth; the AUR repository is a
clone of it.

## Releasing a new version

1. Bump `project(mcu-studio VERSION x.y.z)` in the top-level `CMakeLists.txt`,
   commit, then tag and push:

   ```sh
   git tag vx.y.z && git push --tags
   ```

   CI checks the tag against the CMake version and refuses to release on a
   mismatch, then builds the Windows installer and the AppImage and creates
   the GitHub release.

2. Update this directory:

   ```sh
   cd packaging/aur
   sed -i "s/^pkgver=.*/pkgver=x.y.z/" PKGBUILD
   updpkgsums                            # fills sha256sums from the new tag
   makepkg --printsrcinfo > .SRCINFO
   ```

3. Test the build in a clean chroot, then push `PKGBUILD` and `.SRCINFO` to the
   AUR repository:

   ```sh
   makepkg -si                           # or: extra-x86_64-build
   git -C /path/to/aur-repo commit -am "Update to x.y.z" && git -C /path/to/aur-repo push
   ```

`sha256sums=('SKIP')` is a placeholder for the initial import. `updpkgsums`
replaces it with the real hash of the release tarball, and it should not be
committed as `SKIP` once the first tag exists.
