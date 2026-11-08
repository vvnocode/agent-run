# Agent Run! 应用。key_guard.h 来自 FoloToy/ai-passport 的 main/,其余为本项目代码。
idf_component_register(
    SRCS "main.c"
         "agent_run/ar_proto.c"
         "agent_run/ar_model.c"
         "agent_run/ar_raster.c"
         "agent_run/ar_sprite.c"
         "agent_run/ar_font_cjk14.c"
         "agent_run/ar_ui.c"
         "agent_run/ar_link.c"
         "agent_run/ar_sound.c"
         "agent_run/ar_settings.c"
         "agent_run/ar_pair.c"
         "agent_run/ar_ble.c"
         "agent_run/ar_capture_math.c"
         "agent_run/ar_capture.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_driver_usb_serial_jtag esp_hw_support esp_timer nvs_flash
)
