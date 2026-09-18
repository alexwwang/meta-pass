# meta-pass bootloader hook 组件(单次会话开机策略的执行层)。
# 强链接约定:hooks.c 必须定义 bootloader_hooks_include()(被子项目
# target_link_libraries(-u bootloader_hooks_include) 引用),否则弱符号
# bootloader_after_init 不会被本文件的强定义覆盖。
idf_component_register(SRCS "hooks.c"
                       REQUIRES bootloader_support esp_rom soc spi_flash)

# 策略纯逻辑与宿主测试共享同一份头文件(单一事实源):main/meta_boot_policy.h
# 相对本组件目录是 ../../main。
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")
