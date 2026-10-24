idf_component_register(SRCS "new_year_star.c" "button.c" "led_music.c" "sensor.c" "settings.c"
                            "status.c" "storage.c" "sysinfo.c" "tracks.c" "web.c" "wifi.c"
                            # Playback: what plays and the sources it plays from
                            "player/player.c" "player/stream.c"
                            "player/dlna.c" "player/airplay.c" "player/spotify.cpp"
                    INCLUDE_DIRS "." "player"
                    REQUIRES esp_driver_gpio esp_driver_i2s esp_driver_uart esp_wifi esp_netif esp_event
                             esp_http_client esp_http_server esp_timer mbedtls nvs_flash dns_server cspot
                             esp_driver_tsens esp_app_format spi_flash
                    EMBED_FILES "../assets/index.html" "../assets/system.html")

# Image of the "storage" partition with the bundled tracks. Not flashed by "idf.py flash",
# so tracks uploaded via the web UI survive firmware updates; load it with "idf.py storage-flash".
littlefs_create_partition_image(storage ../assets/tracks)
