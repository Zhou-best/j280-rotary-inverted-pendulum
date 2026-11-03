idf_component_register(
    SRCS
        "app_main.c"
        "tft_spi.c"
        "blueberry_icon.c"
        "uart_startup.c"
        "wifi.c"
        "espnow_comm.c"
        "i2s_audio.c"
        "ai_chat.c"
        "ai_asr.c"
        "esp_sr_handler.c"
        "local_tts.c"
        "web_server.c"
        "k230_tcp.c"
        "fpga_link.c"
        "chinese_font.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "dashboard.html"
    REQUIRES
        driver
        esp_timer
        nvs_flash
        esp_netif
        esp_event
        esp_wifi
        esp_http_client
        esp_http_server
        mbedtls
        esp-sr
        espressif__cjson
)

# Flash voice data for local TTS (xiaole)
set(VOICE_DATA_FILE "${CMAKE_SOURCE_DIR}/components/esp-sr/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat")
if(EXISTS "${VOICE_DATA_FILE}")
    esptool_py_flash_to_partition(flash "voice_data" "${VOICE_DATA_FILE}")
    message(STATUS "Voice data will be flashed to voice_data partition")
else()
    message(WARNING "Voice data file not found: ${VOICE_DATA_FILE}")
    message(WARNING "Local TTS will NOT work. Run:  python <idf>/components/spiffs/spiffsgen.py ...")
endif()
