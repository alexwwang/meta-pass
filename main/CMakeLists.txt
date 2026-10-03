idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "meta_image.c"
         "meta_name.c"
         "meta_slots.c"
         "meta_seq.c"
         "meta_store.c"
         "meta_sign.c"
         "meta_store_json.c"
         "meta_store_prov.c"
         "meta_store_net.c"
         "meta_store_install.c"
         "meta_install_model.c"
         # dynslot 纯逻辑(分配器/表物化/记录/裁决;bootloader hook 与 host
         # 测试链接同一份源,见 bootloader_components/meta_boot_hooks/CMakeLists.txt)
         "meta_carve.c"
         "meta_carve_store.c"
         "meta_carve_boot.c"
         # 启动器侧胶水(ensure/提交/物化/凭据搬移;裸 esp_flash_* 不依赖分区
         # 表缓存,app_main 第一动作就是 ensure,见 meta_carve_flash.h)
         "meta_carve_flash.c"
         "meta_backup.c"
    INCLUDE_DIRS "."
    # 相比基线:去掉 bt(启动器不用 BLE),保留 esp_http_server(配网页 +
    # 本地安装服务)、app_update/esp_partition(槽位与回滚)、mbedtls(install.c
    # 引 mbedtls/sha256.h,配网页也用其 RNG);WAN 下载通道已删,不再需要
    # esp_http_client / esp-tls / esp_crt_bundle(证书包随通道一并移除)。
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             esp_http_server app_update esp_partition mbedtls
             lwip      # install httpd 的 socket 层(自 v5.1 起需显式声明)
             spi_flash # 裸 flash 凭证备份(esp_flash_read/write/erase)
)

# BUG-14 教训:UB 警告(越界写)在构建日志里躺了三个版本没人看 —— 本组件
# 警告一律升级为错误,固件构建不再可能带着未处置的警告出门。
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
