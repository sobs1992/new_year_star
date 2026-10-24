# cspot (Spotify Connect) with the parts of bell it needs, built as one IDF component.
# See README.md for where the sources come from and what was changed.

file(GLOB CSPOT_SRCS
    "cspot/src/*.cpp"
    "cspot/protobuf/*.pb.c"
    "bell/main/utilities/*.cpp" "bell/main/utilities/*.c"
    "bell/main/io/*.cpp" "bell/main/io/*.c"
    "bell/main/platform/esp/*.cpp"
    "bell/external/nanopb/pb_*.c"
    "esp_port/*.c" "esp_port/*.cpp")
file(GLOB TREMOR_SRCS "bell/external/tremor/*.c")

idf_component_register(SRCS ${CSPOT_SRCS} ${TREMOR_SRCS}
                       INCLUDE_DIRS "esp_port" "cspot/include" "cspot"
                                    "bell/main/utilities/include" "bell/main/io/include"
                                    "bell/main/platform"
                                    "bell/external/nanopb" "bell/external/tremor"
                       REQUIRES mbedtls esp-tls pthread lwip freertos esp_timer
                       PRIV_REQUIRES esp_hw_support heap
                       LDFRAGMENTS "linker.lf")

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    PB_ENABLE_MALLOC PB_FIELD_32BIT
    BELL_ONLY_CJSON BELL_DISABLE_FMT BELL_DISABLE_REGEX BELL_DISABLE_CODECS)

# Vorbis decoding runs for every sample: optimise it even in a debug build.
# esp_audio_codec has its own Tremor: ours is renamed (cspot_ prefix) to link next to it.
set_source_files_properties(${CSPOT_SRCS} PROPERTIES COMPILE_OPTIONS "-O2")
set_source_files_properties(${TREMOR_SRCS} PROPERTIES COMPILE_OPTIONS
    "-O2;-include;${CMAKE_CURRENT_LIST_DIR}/bell/external/tremor/cspot_prefix.h")

# Third-party code: its warnings are not ours to fix
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error
    -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function
    -Wno-unused-const-variable -Wno-format -Wno-missing-field-initializers
    -Wno-deprecated-declarations -Wno-maybe-uninitialized -Wno-char-subscripts
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>)
