idf_component_register(
    SRCS "kvm_notify.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
    PRIV_REQUIRES
        esp_http_client
        esp-tls
        esp_timer
        kvm_config
        kvm_log
        kvm_screentext
        video_pipeline
)
