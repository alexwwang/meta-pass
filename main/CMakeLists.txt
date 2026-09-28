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
         "meta_store_analysis.c"
         "meta_store_prov.c"
         "meta_store_idedit.c"
         "meta_store_api.c"
                "meta_store_api_fail.c"
         "meta_store_net.c"
    INCLUDE_DIRS "."
    # 相比基线:去掉 bt(启动器不用 BLE),保留 esp_http_server(配网页)、
    # app_update/esp_partition(槽位与回滚)、mbedtls(SHA-256);
    # 商店下载通道新增 esp_http_client(底层 esp-tls 自动带入)。
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             esp_http_server esp_http_client app_update esp_partition mbedtls
             esp-tls   # r10.5:esp_crt_bundle(自定义证书包 main/certs 的构建集成)
)

# BUG-14 教训:UB 警告(越界写)在构建日志里躺了三个版本没人看 —— 本组件
# 警告一律升级为错误,固件构建不再可能带着未处置的警告出门。
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
