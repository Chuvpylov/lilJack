#!/usr/bin/env bash
# Isolated native checks: no live agents, network, services, or GPU.
set -euo pipefail
export SDL_VIDEODRIVER=dummy
# Suites act as many sessions; a tile-launched shell must not bind them to one.
unset LILJACK_WORKSPACE_SESSION
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd -- "$repo_dir"
hui_dir=${LILJACK_HUI:-"$repo_dir/vendor/hui"}
flags=(-std=c11 -O1 -g -Wall -Wextra -Wno-misleading-indentation)
if [[ ${1:-} == --sanitize ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    shift
fi
if (($#)); then
    printf 'Usage: %s [--sanitize]\n' "$0" >&2
    exit 2
fi
for dependency in gcc pkg-config ffmpeg; do
    command -v "$dependency" >/dev/null || { printf 'Missing test dependency: %s\n' "$dependency" >&2; exit 1; }
done
pkg-config --exists sdl2 freetype2 json-c
read -r -a cflags <<< "$(pkg-config --cflags sdl2 freetype2 json-c)"
read -r -a libs <<< "$(pkg-config --libs sdl2 freetype2 json-c)"
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/liljack-native-tests.XXXXXXXX")
trap 'rm -rf -- "$test_dir"' EXIT
objects=()
modules=(c_theme c_dash c_dock c_blocks c_render c_media c_popup c_ansi owkterm_vt c_review c_metrics c_sixel c_canvas)
if [[ -f liljack_app/c_git_dag.c && -f liljack_app/c_git_dag.h ]]; then
    modules+=(c_git_dag); flags+=(-DLJ_GIT_RENDERER)
fi
for module in "${modules[@]}"; do
    gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" -c "liljack_app/$module.c" -o "$test_dir/$module.o"
    objects+=("$test_dir/$module.o")
done
gcc "${flags[@]}" "${cflags[@]}" tests/test_liljack_theme.c "$test_dir/c_theme.o" "${libs[@]}" -o "$test_dir/theme"
"$test_dir/theme"
gcc "${flags[@]}" "${cflags[@]}" tests/test_liljack_theme_settings.c "$test_dir/c_theme.o" "${libs[@]}" -o "$test_dir/theme-settings"
"$test_dir/theme-settings"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_full_theme.c "$test_dir/c_theme.o" "$test_dir/c_render.o" "${libs[@]}" -lm -o "$test_dir/full-theme"
"$test_dir/full-theme"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_theme_consumers.c "$test_dir/c_theme.o" "$test_dir/c_sixel.o" "${libs[@]}" -lm -ldl -lutil -o "$test_dir/theme-consumers"
"$test_dir/theme-consumers"
gcc "${flags[@]}" tests/test_liljack_dock.c "$test_dir/c_dock.o" -o "$test_dir/dock"
gcc "${flags[@]}" -I liljack_app tests/test_liljack_blocks.c "$test_dir/c_blocks.o" -o "$test_dir/blocks"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_native.c "${objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/native"
header_objects=()
for object in "${objects[@]}"; do [[ $object == */c_metrics.o || $object == */c_ansi.o ]] || header_objects+=("$object"); done
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_header.c "${header_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/header"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_graph_columns.c "${header_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/graph-columns"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_stop_menu.c "${objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/stop-menu"
"$test_dir/stop-menu"
"$test_dir/graph-columns"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/ansi_graph_wire_probe.c "$test_dir/c_sixel.o" "$test_dir/c_theme.o" "${libs[@]}" -lm -o "$test_dir/graph-wire"
"$test_dir/graph-wire"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_ansi_sync_fence.c "$test_dir/c_sixel.o" "$test_dir/c_theme.o" "${libs[@]}" -lm -lutil -o "$test_dir/ansi-sync-fence"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_image_slots.c "$test_dir/c_sixel.o" "$test_dir/c_theme.o" "${libs[@]}" -lm -lutil -o "$test_dir/image-slots"
"$test_dir/ansi-sync-fence"
"$test_dir/image-slots"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_strip_occlusion.c "$test_dir/c_sixel.o" "$test_dir/c_theme.o" "${libs[@]}" -lm -lutil -o "$test_dir/strip-occlusion"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_strip_emoji.c "$test_dir/c_sixel.o" "$test_dir/c_theme.o" "${libs[@]}" -lm -lutil -o "$test_dir/strip-emoji"
"$test_dir/strip-occlusion"
"$test_dir/strip-emoji"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_click_controls.c "${objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/click-controls"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_session_wide_bg.c "${objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/session-wide-bg"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_divider_controls.c "${objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/divider-controls"
canvas_objects=()
for object in "${objects[@]}"; do [[ $object == */c_ansi.o ]] || canvas_objects+=("$object"); done
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_divider_canvas.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/divider-canvas"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_paste_input.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/paste-input"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_popups.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/popups"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_popup_chrome.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/popup-chrome"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_logo_menu.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/logo-menu"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_tile_rule.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/tile-rule"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_tab_overflow.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/tab-overflow"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_tab_autoresize.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/tab-autoresize"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_theme_editor.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/theme-editor"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_hover_rule.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/hover-rule"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_drag.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/drag"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_dash.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/dash"
"$test_dir/dash"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_folder_picker.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/folder-picker"
"$test_dir/folder-picker"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_room_start_once.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/room-start-once"
"$test_dir/room-start-once"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_actionable_scroll.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/actionable-scroll"
"$test_dir/actionable-scroll"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_label_form_stability.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/label-form"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_tabs.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/tabs"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_selection.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/selection"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_paste.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/paste"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_selection_scroll.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/selection-scroll"
"$test_dir/tabs"
"$test_dir/selection"
"$test_dir/paste"
"$test_dir/selection-scroll"
"$test_dir/label-form"
"$test_dir/drag"
"$test_dir/popups"
"$test_dir/popup-chrome"
"$test_dir/logo-menu"
"$test_dir/tile-rule"
"$test_dir/tab-overflow"
"$test_dir/tab-autoresize"
"$test_dir/theme-editor"
"$test_dir/hover-rule"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_status_elide.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/status-elide"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_icon_width.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/icon-width"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_border_canvas.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/border-canvas"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_ring_gutter.c "${canvas_objects[@]}" "${libs[@]}" -lutil -lm -o "$test_dir/ring-gutter"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_canvas_render.c "$test_dir/c_canvas.o" "${libs[@]}" -lm -o "$test_dir/canvas-render"
"$test_dir/canvas-render"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_canvas_image.c "$test_dir/c_canvas.o" "${libs[@]}" -lm -o "$test_dir/canvas-image"
"$test_dir/canvas-image"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_canvas_damage.c "$test_dir/c_canvas.o" "${libs[@]}" -lm -o "$test_dir/canvas-damage"
"$test_dir/canvas-damage"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_canvas_edit.c "$test_dir/c_canvas.o" "${libs[@]}" -lm -o "$test_dir/canvas-edit"
"$test_dir/canvas-edit"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_canvas_fullscreen.c "$test_dir/c_ansi.o" "$test_dir/c_sixel.o" "$test_dir/c_theme.o" "$test_dir/owkterm_vt.o" "${libs[@]}" -lutil -lm -o "$test_dir/canvas-fullscreen"
"$test_dir/canvas-fullscreen"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_sixel_damage.c "$test_dir/c_ansi.o" "$test_dir/c_sixel.o" "$test_dir/c_theme.o" "${libs[@]}" -lutil -lm -o "$test_dir/sixel-damage"
"$test_dir/sixel-damage"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_media.c "$test_dir/c_render.o" "$test_dir/c_media.o" "${libs[@]}" -lm -o "$test_dir/media"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_media_delegate.c "$test_dir/c_media.o" "${libs[@]}" -lm -o "$test_dir/media-delegate"
"$test_dir/media-delegate"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_popup.c "$test_dir/c_render.o" "$test_dir/c_media.o" "$test_dir/c_popup.o" "${libs[@]}" -lm -o "$test_dir/popup"
gcc "${flags[@]}" "${cflags[@]}" tests/test_liljack_ansi.c "$test_dir/c_ansi.o" "$test_dir/c_sixel.o" -lutil "$test_dir/c_theme.o" "${libs[@]}" -o "$test_dir/ansi"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_vs16_width.c "$test_dir/c_sixel.o" -lutil "$test_dir/c_theme.o" "${libs[@]}" -o "$test_dir/vs16-width"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/ansi_lab.c "$test_dir/c_ansi.o" "$test_dir/c_sixel.o" -lm "$test_dir/c_theme.o" "${libs[@]}" -o "$test_dir/ansi-lab"
"$test_dir/ansi-lab" --help
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/ansi_lab_phase_probe.c "$test_dir/c_sixel.o" "$test_dir/owkterm_vt.o" -lm "$test_dir/c_theme.o" "${libs[@]}" -o "$test_dir/ansi-phase"
"$test_dir/ansi-phase"
gcc "${flags[@]}" -I liljack_app "${cflags[@]}" tests/render_text_trim_proof.c "$test_dir/c_render.o" "${libs[@]}" -lm -o "$test_dir/text-trim"
gcc "${flags[@]}" -I "$hui_dir" tests/test_liljack_metrics_display.c -ldl -lm -o "$test_dir/metrics-display"
gcc "${flags[@]}" -I liljack_app tests/test_liljack_metrics.c "$test_dir/c_metrics.o" -ldl -lm -o "$test_dir/metrics"
gcc "${flags[@]}" -I liljack_app tests/test_liljack_metrics_buckets.c "$test_dir/c_metrics.o" -ldl -lm -o "$test_dir/metrics-buckets"
gcc "${flags[@]}" -I liljack_app tests/test_liljack_metrics_cores.c "$test_dir/c_metrics.o" -ldl -lm -o "$test_dir/metrics-cores"
gcc "${flags[@]}" -I liljack_app tests/test_liljack_sixel.c "$test_dir/c_sixel.o" -o "$test_dir/sixel"
gcc "${flags[@]}" -I liljack_app tests/test_liljack_vt_pixels.c "$test_dir/owkterm_vt.o" -o "$test_dir/vt-pixels"
gcc "${flags[@]}" -I liljack_app tests/test_liljack_vt_sync.c "$test_dir/owkterm_vt.o" -o "$test_dir/vt-sync"
gcc "${flags[@]}" -I liljack_app tests/test_liljack_vt_osc52.c "$test_dir/owkterm_vt.o" -o "$test_dir/vt-osc52"
gcc "${flags[@]}" -I liljack_app "${cflags[@]}" tests/test_liljack_ansi_image.c "$test_dir/c_ansi.o" "$test_dir/c_sixel.o" "${libs[@]}" -lutil "$test_dir/c_theme.o" -o "$test_dir/ansi-image"
ffmpeg -nostdin -hide_banner -loglevel error -threads 2 -filter_threads 1 \
    -f lavfi -i 'color=c=red:s=64x36:r=12' -t 0.5 -an -c:v mpeg4 -threads 2 -pix_fmt yuv420p "$test_dir/red.mp4"
"$test_dir/dock"
"$test_dir/blocks"
"$test_dir/ansi"
"$test_dir/vs16-width"
"$test_dir/text-trim"
"$test_dir/metrics"
"$test_dir/metrics-buckets"
"$test_dir/metrics-cores"
"$test_dir/metrics-display"
"$test_dir/sixel"
"$test_dir/vt-pixels"
"$test_dir/vt-sync"
"$test_dir/vt-osc52"
"$test_dir/ansi-image"
gcc "${flags[@]}" -I liljack_app "${cflags[@]}" tests/test_liljack_overlay_trace.c "$test_dir/c_ansi.o" "$test_dir/c_sixel.o" "${libs[@]}" -lutil "$test_dir/c_theme.o" -o "$test_dir/overlay-trace"
"$test_dir/overlay-trace"
"$test_dir/native"
"$test_dir/header"
"$test_dir/click-controls"
"$test_dir/session-wide-bg"
gcc "${flags[@]}" -I "$hui_dir" "${cflags[@]}" tests/test_liljack_border_output.c "$test_dir/c_sixel.o" "$test_dir/c_theme.o" "${libs[@]}" -o "$test_dir/border-output"
"$test_dir/border-output"
"$test_dir/divider-controls"
"$test_dir/divider-canvas"
"$test_dir/border-canvas"
"$test_dir/ring-gutter"
"$test_dir/status-elide"
"$test_dir/icon-width"
"$test_dir/paste-input"
if [[ -f "$test_dir/c_review.o" && -f tests/test_liljack_review_render.c ]]; then
    gcc "${flags[@]}" -I liljack_app "${cflags[@]}" tests/test_liljack_review_render.c "$test_dir/c_review.o" "$test_dir/c_render.o" "$test_dir/c_ansi.o" "$test_dir/c_sixel.o" "${libs[@]}" -lm "$test_dir/c_theme.o" -o "$test_dir/review"
    cat > "$test_dir/review.json" <<'JSON'
{"summary":{"total":2,"done":1,"pct":50,"active":1,"queued":0,"blocked":0,"per_agent":{}},"todos":[{"agent":"codex","task":"review scrolling","state":"active","progress":"1/2","mark":">"}],"git":{"rows":[],"error":"isolated test has no repository"},"fit":{"scores":{},"error":"no scores recorded yet"}}
JSON
    "$test_dir/review" "$test_dir/review.json"
fi
"$test_dir/media" "$test_dir/red.mp4" "$test_dir/image.png"
"$test_dir/popup" "$test_dir/red.mp4"
bash tests/run_liljack_python_tests.sh
printf 'All native lilJack tests passed.\n'
# owkTerm app suite (standard apps headless + goldens), opt-in: OWKTERM_APPS=1
if [[ ${OWKTERM_APPS:-0} == 1 && -f "$(dirname "$0")/../../owkTerm/tests/apps.py" ]]; then
    python3 "$(dirname "$0")/../../owkTerm/tests/apps.py" --sizes 800x560,1280x720 --font 14 || { echo 'owkTerm app suite FAILED' >&2; exit 1; }
fi
