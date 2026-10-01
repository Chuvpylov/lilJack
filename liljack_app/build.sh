#!/usr/bin/env bash
# Build one native C executable, cached by source/compiler/dependency content.
set -euo pipefail
app_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$app_dir/.." && pwd)
hui_dir=${LILJACK_HUI:-"$repo_dir/vendor/hui"}
build_dir=${LILJACK_BUILD_ROOT:-"$HOME/.cache/liljack/build"}
mkdir -p -- "$build_dir"
command -v gcc >/dev/null || { echo 'lilJack needs gcc to build its C core.' >&2; exit 1; }
pkg-config --exists sdl2 freetype2 json-c || { echo 'lilJack needs SDL2, FreeType and json-c development packages.' >&2; exit 1; }
test -f "$hui_dir/hui.h" || { echo 'HUI source missing. Set LILJACK_HUI to brain/hui.' >&2; exit 1; }
# owkTerm (the standalone sixel terminal) lives in brain/owkTerm and builds against
# this directory's VT and renderer. These targets only delegate so `./liljack owkterm` keeps working.
if [[ ${1:-} == owkterm || ${1:-} == owkterm-host ]]; then
    owkterm_dir=${OWKTERM_DIR:-"$repo_dir/../owkTerm"}
    test -f "$owkterm_dir/build.sh" || { echo "owkTerm project missing at $owkterm_dir (set OWKTERM_DIR)." >&2; exit 1; }
    target=$1; [[ $target == owkterm-host ]] && target=host || target=owkterm
    LILJACK_DIR="$app_dir" LILJACK_HUI="$hui_dir" exec bash "$owkterm_dir/build.sh" "$target"
elif [[ ${1:-} == canvas-png ]]; then
    # Agents' eyes on the room canvas: c_canvas.c alone, no SDL (liljack room --canvas-png).
    exec 9>"$build_dir/c-build.lock"; flock 9
    digest=$({ sha256sum "$app_dir/canvas_png.c" "$app_dir/c_canvas.c" "$app_dir/c_canvas.h" "$hui_dir"/deps/stb_image*.h; gcc -dumpversion; pkg-config --modversion json-c; } | sha256sum | cut -c1-20)
    target="$build_dir/liljack-canvas-png-$digest"
    if [[ ! -x "$target" ]]; then
        tmp="$target.$$.tmp"; trap 'rm -f -- "$tmp"' EXIT
        read -r -a jflags <<< "$(pkg-config --cflags json-c)"; read -r -a jlibs <<< "$(pkg-config --libs json-c)"
        gcc -std=c11 -O2 -Wall -Wextra -I "$hui_dir" "${jflags[@]}" "$app_dir/canvas_png.c" "$app_dir/c_canvas.c" "${jlibs[@]}" -lm -o "$tmp"
        chmod 700 "$tmp"; mv -- "$tmp" "$target"
    fi
    printf '%s\n' "$target"; exit 0
elif [[ $# != 0 ]]; then
    echo 'Usage: build.sh [owkterm|owkterm-host|canvas-png]' >&2; exit 2
fi
exec 9>"$build_dir/c-build.lock"
flock 9
digest=$({ sha256sum "$app_dir"/*.[ch] "$app_dir/build.sh" "$hui_dir"/*.h "$hui_dir"/backends/*.h "$hui_dir"/deps/stb_image*.h; gcc -dumpversion; pkg-config --modversion sdl2 freetype2 json-c; } | sha256sum | cut -c1-20)
target="$build_dir/liljack-c-$digest"
if [[ ! -x "$target" ]]; then
    tmp="$target.$$.tmp"
    trap 'rm -f -- "$tmp"' EXIT
    echo 'Building lilJack native C app…' >&2
    read -r -a cflags <<< "$(pkg-config --cflags sdl2 freetype2 json-c)"
    read -r -a libs <<< "$(pkg-config --libs sdl2 freetype2 json-c)"
    review_sources=("$app_dir/c_review.c" "$app_dir/c_dash.c"); review_flags=()
    if [[ -f "$app_dir/c_git_dag.c" && -f "$app_dir/c_git_dag.h" ]]; then
        review_sources+=("$app_dir/c_git_dag.c"); review_flags+=(-DLJ_GIT_RENDERER)
    fi
    gcc -std=c11 -O2 -Wall -Wextra -Wno-misleading-indentation -I "$hui_dir" "${cflags[@]}" \
        "${review_flags[@]}" "${review_sources[@]}" \
        "$app_dir/main.c" "$app_dir/c_theme.c" "$app_dir/c_dock.c" "$app_dir/c_blocks.c" "$app_dir/c_render.c" "$app_dir/c_media.c" "$app_dir/c_popup.c" "$app_dir/c_metrics.c" "$app_dir/c_ansi.c" "$app_dir/owkterm_vt.c" "$app_dir/c_sixel.c" "$app_dir/c_canvas.c" \
        "${libs[@]}" -lutil -lm -ldl -o "$tmp"
    chmod 700 "$tmp"
    mv -- "$tmp" "$target"
fi
printf '%s\n' "$target"
