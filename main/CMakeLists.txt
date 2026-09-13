idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "meta_image.c"
         "meta_name.c"
         "meta_slots.c"
         "meta_import.c"
         "meta_store.c"
         "meta_sign.c"
         "meta_net.c"
    INCLUDE_DIRS "."
    # 相比基线:去掉 bt(启动器不用 BLE),新增 esp_http_server(导入页)、
    # app_update/esp_partition(槽位与回滚)、mbedtls(SHA-256)。
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             esp_http_server app_update esp_partition mbedtls
)
