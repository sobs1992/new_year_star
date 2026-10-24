# Activate ESP-IDF environment for this project.
# Usage: . ./set_env.sh   (must be sourced, not executed)

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    echo "Run as: . ./set_env.sh (source it, otherwise the environment is lost)" >&2
    exit 1
fi

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"

if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF not found at $IDF_PATH (set IDF_PATH to override)" >&2
    return 1
fi

. "$IDF_PATH/export.sh" > /dev/null || return 1

export ESPPORT=/dev/ttyACM0

echo "ESP-IDF $(cd "$IDF_PATH" && git describe --tags 2>/dev/null) ready, port $ESPPORT"
