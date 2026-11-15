idf_component_register(
    SRCS "p4camera.c" "qrfilter.c"
    INCLUDE_DIRS "."
    REQUIRES esp_video esp_driver_i2c k_quirc
)
