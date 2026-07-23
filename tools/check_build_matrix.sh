#!/bin/sh

set -eu

make_command=${MAKE:-make}
log_directory=$(mktemp -d "${TMPDIR:-/tmp}/ambient-build-matrix.XXXXXX")
trap 'rm -rf "$log_directory"' EXIT HUP INT TERM

build_configuration()
{
    name=$1
    flags=$2
    output_directory="build-${name}"
    log_file="${log_directory}/${name}.log"

    "$make_command" BUILD_DIR="$output_directory" clean >/dev/null
    "$make_command" --jobs=2 \
        BUILD_DIR="$output_directory" \
        CPPFLAGS_EXTRA="$flags" >"$log_file" 2>&1

    if grep -En 'warning:|error:' "$log_file"; then
        echo "Build produced diagnostics: ${name}" >&2
        return 1
    fi

    arm-none-eabi-size "${output_directory}/ambient-tv-lighting.elf" |
        tail -n 1
}

build_configuration release ""
build_configuration diagnostics \
    "-DAPP_ENABLE_DCMI_DIAGNOSTICS=1 -DAPP_ENABLE_COLOR_DIAGNOSTICS=1 -DAPP_ENABLE_WS2812_DIAGNOSTICS=1"

for mode in 0 1 2 3; do
    build_configuration "source-${mode}" "-DAPP_VIDEO_SOURCE_MODE=${mode}"
done

build_configuration signal-diagnostic \
    "-DAPP_ENABLE_TFP401_SIGNAL_DIAGNOSTIC=1"
build_configuration led-self-test \
    "-DAPP_ENABLE_LED_SELF_TEST=1"
build_configuration signal-monitor \
    "-DAPP_ENABLE_TFP401_SIGNAL_MONITOR=1"
