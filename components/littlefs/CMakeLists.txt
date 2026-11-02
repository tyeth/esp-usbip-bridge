idf_component_register(SRCS "lfs.c" "lfs_util.c"
                       INCLUDE_DIRS ".")

# LFS_MULTIVERSION adds lfs_config.disk_version, so a DUT's file system is
# written at the on-disk version its firmware reads (public: it changes the
# struct the users of lfs.h see).  No asserts: a corrupt file system on a
# device must not reboot the bridge.
target_compile_definitions(${COMPONENT_LIB} PUBLIC LFS_MULTIVERSION LFS_NO_ASSERT LFS_NO_DEBUG)
