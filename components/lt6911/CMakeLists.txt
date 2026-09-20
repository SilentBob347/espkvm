idf_component_register(
    SRCS "lt6911.c"
    REQUIRES
        esp_driver_i2c
        kvm_bridge
        esp_timer
)

# Same as the TC358743: nothing calls this by name, so without the -u the linker
# drops the object and the driver registers itself nowhere.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u kvm_bridge_reg_lt6911")
