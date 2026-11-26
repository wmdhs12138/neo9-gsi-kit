#!/system/bin/sh
# Read-only display / refresh-rate / RM692H5 LTPO diagnostic probe for PD2338.
# Usage: su -c 'sh display/probe_refresh.sh [--sample]'
set -u

if [ "$(id -u)" != 0 ]; then
    printf '%s\n' 'Run with su to read panel sysfs nodes and display services.' >&2
    exit 1
fi

SAMPLE_DDIC=0
if [ "${1:-}" = "--sample" ]; then
    SAMPLE_DDIC=1
elif [ "$#" -gt 0 ]; then
    printf '%s\n' 'Usage: sh probe_refresh.sh [--sample]' >&2
    exit 2
fi

read_node() {
    printf '\n[%s]\n' "$1"
    if [ ! -r "$1" ]; then
        printf '%s\n' 'Unavailable'
    elif value=$(/system/bin/timeout 3 cat "$1" 2>/dev/null); then
        printf '%s\n' "$value"
    else
        printf '%s\n' 'Read failed or timed out'
    fi
}

run_service_cmd() {
    local output result
    output=$("$@" </dev/null 2>&1)
    result=$?
    if [ -n "$output" ]; then
        printf '%s\n' "$output"
    fi
    return "$result"
}

printf '[Build]\n'
for prop in ro.product.vendor.device ro.build.version.release ro.build.version.sdk ro.gsid.image_running; do
    printf '%s=%s\n' "$prop" "$(getprop "$prop")"
done

for node in /sys/lcm/panel_name /sys/class/drm/card0-DSI-1/dpms /sys/lcm/bl_level \
    /sys/lcm/ltpo_mode /sys/lcm/refresh_monitor /sys/lcm/aod_disp_info \
    /sys/class/drm/card0-DSI-1/modes; do
    read_node "$node"
done

if [ "$(cat /sys/class/drm/card0-DSI-1/dpms 2>/dev/null)" = On ] &&
   [ "$(cat /sys/lcm/bl_level 2>/dev/null || echo 0)" -gt 0 ] 2>/dev/null; then
    read_node /sys/lcm/lcm_fps
fi

if [ "$SAMPLE_DDIC" = 1 ]; then
    printf '\n[DDIC refresh_monitor samples]\n'
    prev_mon="$(cat /sys/lcm/refresh_monitor 2>/dev/null || echo 0)"
    enabled_mon=0
    if [ "$prev_mon" = 0 ] && printf '1\n' > /sys/lcm/refresh_monitor 2>/dev/null; then
        enabled_mon=1
    fi
    for i in 1 2 3 4 5 6; do
        printf 'sample=%s dpms=%s bl_level=%s ddic_hz=%s\n' "$i" \
            "$(cat /sys/class/drm/card0-DSI-1/dpms 2>/dev/null)" \
            "$(cat /sys/lcm/bl_level 2>/dev/null)" \
            "$(/system/bin/timeout 3 cat /sys/lcm/refresh_monitor 2>/dev/null)"
        sleep 1
    done
    if [ "$enabled_mon" = 1 ]; then
        printf '0\n' > /sys/lcm/refresh_monitor 2>/dev/null || true
    fi
fi

printf '\n[DisplayManager]\n'
for key in min_refresh_rate peak_refresh_rate; do
    printf '%s=%s\n' "$key" "$(run_service_cmd settings --user current get system "$key")"
done
printf 'match_content_frame_rate=%s\n' "$(run_service_cmd settings --user current get secure match_content_frame_rate)"
run_service_cmd cmd display get-match-content-frame-rate-pref
run_service_cmd cmd display get-user-preferred-display-mode 0
dumpsys -t 5 display | grep -E 'mActiveModeId=|mActiveSfDisplayMode=|mDefaultModeId=|mUserPreferredModeId=|mModeSwitchingType:|^[[:space:]]+DisplayMode\{id=|PRIORITY_.*(REFRESH_RATE|FRAME_RATE|UDFPS)'

printf '\n[SurfaceFlinger and HWC]\n'
dumpsys -t 5 SurfaceFlinger | grep -Ei 'activeMode=|desiredMode=|hwcId=|allowGroupSwitching=|idleTimer=|KernelIdleTimer=|ContentDetection=|active config index|num configs:|vsync_period'
