# meta-pass bootloader hook 组件(单次会话开机策略 + dynslot 表强制的执行层)。
# 强链接约定:hooks.c 必须定义 bootloader_hooks_include()(被子项目
# target_link_libraries(-u bootloader_hooks_include) 引用),否则弱符号
# bootloader_after_init 不会被本文件的强定义覆盖。
#
# dynslot:carve 裁决/记录编解码/分区表物化/MD5 是纯逻辑源,与启动器、host
# 测试共用同一份文件(单一事实源,设计 §4.4);这里以绝对路径拉进 bootloader
# 链接面,头文件同样来自 ../../main。
set(MP_MAIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../main")

idf_component_register(SRCS
                       "hooks.c"
                       "${MP_MAIN_DIR}/meta_carve_boot.c"
                       "${MP_MAIN_DIR}/meta_carve_store.c"
                       "${MP_MAIN_DIR}/meta_carve.c"
                       "${MP_MAIN_DIR}/meta_md5.c"
                       REQUIRES bootloader_support esp_rom soc spi_flash)

# 策略纯逻辑与宿主测试共享同一份头文件(单一事实源):main/meta_boot_policy.h
# 相对本组件目录是 ../../main。
target_include_directories(${COMPONENT_LIB} PRIVATE "${MP_MAIN_DIR}")
# The ESP32-C3 second-stage bootloader has an on-chip MD5 implementation;
# use it to keep the bootloader within its tight DRAM/rodata budget.
target_compile_definitions(${COMPONENT_LIB} PRIVATE META_MD5_USE_ROM=1)
