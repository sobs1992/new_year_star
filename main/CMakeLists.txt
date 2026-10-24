idf_component_register(SRCS "new_year_star.c" "airplay.c" "button.c" "dlna.c" "player.c" "led_music.c"
                            "radio.c" "sensor.c"
                            "settings.c" "status.c" "storage.c" "web.c" "wifi.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_i2s esp_driver_uart esp_wifi esp_netif esp_event
                             esp_http_client esp_http_server esp_timer mbedtls nvs_flash dns_server
                    EMBED_FILES "../assets/index.html")

# Image of the "storage" partition with the bundled tracks. Not flashed by "idf.py flash",
# so tracks uploaded via the web UI survive firmware updates; load it with "idf.py storage-flash".
littlefs_create_partition_image(storage ../assets/tracks)
