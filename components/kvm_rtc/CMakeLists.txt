idf_component_register(
    SRCS "kvm_rtc.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES freertos kvm_config
)
