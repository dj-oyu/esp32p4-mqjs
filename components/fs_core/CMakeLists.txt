# fs_core: ボリューム登録簿 + 仮想パス解決 + ボード非依存のファイル操作。
#
# ここには SD も LittleFS も出てこない (POSIX VFS しか使わない) ので、
# Kconfig による出し分けが要らない。ボード差は「誰がボリュームを登録するか」
# だけで表現される — docs/filer-storage-design.md §3。
idf_component_register(SRCS "fs_vol.c" "fs_ops.c"
                       INCLUDE_DIRS "include")
