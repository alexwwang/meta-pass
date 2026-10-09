idf_component_register(
    SRCS "test_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_partition esp_app_format esp_driver_usb_serial_jtag esp_timer esp_system freertos mbedtls esp_common
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror)
