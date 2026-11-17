#!/usr/bin/env bash
# Runs Backtrack for real on a virtual X11 screen (Xvfb) with a virtual sound card (a PulseAudio
# null sink) and checks the clips it saves. CI runs it on the plain build and on the AppImage.
#
#   smoke-test.sh run <command that starts backtrack>   records ~14 s, saves 3 clips
#   smoke-test.sh check                                  each clip: H.264 + AAC, sensible length,
#                                                        and the trimmed clip the run made
#
# The run saves twice by itself (--save-after 6 and 12) and once at about 9 s through a second
# start with --save (the single-instance path a desktop shortcut uses). Needs Xvfb, pulseaudio, pactl; check needs ffprobe.
set -euo pipefail

work=${BT_SMOKE_DIR:-/tmp/backtrack-smoke}
clips=$work/home/Videos/Backtrack

run() {
    rm -rf "$work"
    mkdir -p "$work/home" "$work/runtime"
    chmod 700 "$work/runtime"
    export HOME=$work/home XDG_RUNTIME_DIR=$work/runtime
    export XDG_CONFIG_HOME=$work/home/.config XDG_STATE_HOME=$work/home/.local/state XDG_DATA_HOME=$work/home/.local/share
    unset WAYLAND_DISPLAY XDG_SESSION_TYPE QT_QPA_PLATFORM
    export DISPLAY=:77

    Xvfb :77 -screen 0 1280x720x24 -nolisten tcp >"$work/xvfb.log" 2>&1 &
    local xvfb=$!
    pulseaudio -n --daemonize=yes --exit-idle-time=-1 --log-target=file:"$work/pulse.log" \
        --load=module-native-protocol-unix --load="module-null-sink sink_name=smoke" --load=module-always-sink
    for _ in $(seq 50); do
        pactl info >/dev/null 2>&1 && [ -e /tmp/.X11-unix/X77 ] && break
        sleep 0.2
    done
    pactl set-default-sink smoke
    pactl set-default-source smoke.monitor

    # The single-instance socket (QLocalServer in src/linux/main.cpp). An AppImage start first
    # unpacks itself, which takes a while: the timing below counts from when the app is up.
    local socket=${TMPDIR:-/tmp}/backtrack-$(id -u)
    rm -f "$socket"
    "$@" --save-after 6 --save-after 12 --exit-after 17 >"$work/backtrack.log" 2>&1 &
    local app=$!
    for _ in $(seq 120); do
        [ -S "$socket" ] && break
        kill -0 "$app" 2>/dev/null || break
        sleep 0.5
    done
    [ -S "$socket" ] || { cat "$work/backtrack.log"; echo "Backtrack did not start"; exit 1; }
    sleep 9
    # A second start with --save hands the save to the running one and returns at once
    # (between the two scheduled saves, so it is never dropped as "a save is still running").
    "$@" --save
    local status=0
    wait "$app" || status=$?

    # The library and the clip page on the first clip (while the sound server still runs: the
    # player plays into it). Reads it with the app's own FFmpeg, so on Arch the AppImage's.
    local media=0
    if [ "$status" -eq 0 ]; then
        local saved=("$clips"/*.mp4)
        "$@" --check-media "${saved[0]}" "$work/trim.mp4" >"$work/media.log" 2>&1 || media=$?
    fi

    pulseaudio --kill || true
    kill "$xvfb" 2>/dev/null || true
    echo "---- backtrack output"
    cat "$work/backtrack.log"
    if [ "$status" -ne 0 ]; then
        echo "Backtrack exited with $status"
        exit 1
    fi
    grep -q "Video encoder:" "$work/backtrack.log" || { echo "No video encoder opened"; exit 1; }
    grep -q "Sound: " "$work/backtrack.log" || { echo "Sound did not start"; exit 1; }
    echo "---- media check"
    cat "$work/media.log"
    [ "$media" -eq 0 ] || { echo "Reading the clips like the library does failed"; exit 1; }
    # Every save reached the library (also the one handed over by `backtrack --save`).
    local library=$XDG_DATA_HOME/Backtrack/library.json
    local known
    known=$(grep -c '"file":' "$library" || true)
    [ "$known" -eq 3 ] || { cat "$library"; echo "library.json knows $known clips, not 3"; exit 1; }
}

check() {
    shopt -s nullglob
    local files=("$clips"/*.mp4)
    local leftovers=("$clips"/.*.part)
    [ "${#leftovers[@]}" -eq 0 ] || { echo "Half-written files left: ${leftovers[*]}"; exit 1; }
    [ "${#files[@]}" -eq 3 ] || { echo "Expected 3 clips, found ${#files[@]}: ${files[*]}"; exit 1; }
    for clip in "${files[@]}"; do
        local video audio duration
        video=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height -of csv=p=0 "$clip")
        audio=$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name,sample_rate,channels -of csv=p=0 "$clip")
        duration=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$clip")
        echo "$(basename "$clip"): video $video, audio $audio, $duration s"
        [ "$video" = "h264,1280,720" ] || { echo "Wrong video stream"; exit 1; }
        [ "$audio" = "aac,48000,2" ] || { echo "Wrong sound stream"; exit 1; }
        awk -v d="$duration" 'BEGIN { exit !(d >= 2 && d <= 17) }' || { echo "Unexpected length"; exit 1; }
        # Every frame must decode.
        ffmpeg -v error -xerror -i "$clip" -f null - || { echo "The clip does not decode cleanly"; exit 1; }
    done
    local trim=$work/trim.mp4
    [ -f "$trim" ] || { echo "No trimmed clip"; exit 1; }
    local video audio duration
    video=$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height -of csv=p=0 "$trim")
    audio=$(ffprobe -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$trim")
    duration=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$trim")
    echo "trim.mp4: video $video, audio $audio, $duration s"
    [ "$video" = "h264,1280,720" ] && [ "$audio" = "aac" ] || { echo "The trimmed clip lost a stream"; exit 1; }
    awk -v d="$duration" 'BEGIN { exit !(d >= 2.4 && d <= 3.7) }' || { echo "The trimmed clip has the wrong length"; exit 1; }
    ffmpeg -v error -xerror -i "$trim" -f null - || { echo "The trimmed clip does not decode cleanly"; exit 1; }
    echo "All clips fine"
}

case "${1:-}" in
run) shift; run "$@" ;;
check) check ;;
*) echo "Usage: $0 run <backtrack command> | check"; exit 2 ;;
esac
