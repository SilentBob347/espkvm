idf_component_register(
    SRCS "kvm_cec.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        video_pipeline
        kvm_bridge
        kvm_config
        esp_timer
)
