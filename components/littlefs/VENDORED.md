# littlefs (vendored)

Source: https://github.com/littlefs-project/littlefs **v2.11** (`LFS_VERSION`
0x0002000b, on-disk version 2.1), BSD-3-Clause (see LICENSE.md), taken from
the copy arduino-pico 5.5.1 ships (`libraries/LittleFS/lib/littlefs`).

Only `lfs.c`, `lfs.h`, `lfs_util.c` and `lfs_util.h` are copied, unchanged.
Build options are in `CMakeLists.txt`: `LFS_MULTIVERSION` (write a device's
file system at its own on-disk version), `LFS_NO_ASSERT`, `LFS_NO_DEBUG`.

Used by `main/dut_fs.c` to browse and write LittleFS on a device's flash
through its bootloader.  There is no `idf_component.yml` on purpose: this is
a local component, not a managed dependency.

To update: replace the four files from a new release and check the
`lfs_config` fields `main/dut_fs.c` sets.
