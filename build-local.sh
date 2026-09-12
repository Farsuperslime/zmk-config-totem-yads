#!/usr/bin/env bash
#
# Local firmware build for this ZMK config.
#
#   ./build-local.sh                     build every entry in build.yaml
#   ./build-local.sh totem-miryoku-dongle-prospector
#                                        build just that artifact
#   ./build-local.sh --update            force `west update` first
#   ./build-local.sh --help
#
# Artefacts land in $ZMK_WORKSPACE/artifacts/ (both .uf2 and .bin are copied
# when present). Each build.yaml entry is built exactly as written: same board,
# shield, snippet and cmake-args, same -DZMK_CONFIG / -DZMK_EXTRA_MODULES.
#
# This script lives in the repo, so run it from anywhere. Everything it needs
# lives outside the repo and can be pointed elsewhere with these variables:
#
#   ZMK_WORKSPACE           west workspace        (default ~/Documents/totem-zmk/build)
#   ZEPHYR_SDK_INSTALL_DIR  Zephyr SDK            (default ~/zephyr-sdk-0.16.9)
#   ZMK_VENV                python venv           (default ~/.venvs/zmk)

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS="${ZMK_WORKSPACE:-$HOME/Documents/totem-zmk/build}"
SDK="${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-0.16.9}"
VENV="${ZMK_VENV:-$HOME/.venvs/zmk}"
OUT="$WS/artifacts"
PY="$VENV/bin/python"

die() { printf 'build-local: error: %s\n' "$*" >&2; exit 1; }
info() { printf '\033[1m==>\033[0m %s\n' "$*"; }

usage() { sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

want_update=0
selected=()
for arg in "$@"; do
    case "$arg" in
        -h|--help) usage ;;
        -u|--update) want_update=1 ;;
        -*) die "unknown option: $arg" ;;
        *) selected+=("$arg") ;;
    esac
done

# --- sanity checks ---------------------------------------------------------
[[ -n "$WS" && "$WS" != "/" ]] || die "refusing to use ZMK_WORKSPACE='$WS'"
[[ -d "$WS/.west" ]] || die "no west workspace at $WS (run: west init -l $WS/config && west update)"
[[ -d "$SDK/arm-zephyr-eabi" ]] || die "no Zephyr SDK toolchain at $SDK"
[[ -x "$PY" ]] || die "no python at $PY (create the venv and install zephyr/scripts/requirements-base.txt)"
command -v west >/dev/null || die "west is not on PATH"
[[ -f "$REPO/build.yaml" ]] || die "no build.yaml next to $REPO"

export ZEPHYR_SDK_INSTALL_DIR="$SDK"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export PATH="$VENV/bin:$PATH"

# --- keep the workspace's copy of config/ in step with the repo ------------
# config/ doubles as the west manifest repository, so it has to live inside
# the workspace; copy it fresh each run so deletions are picked up too.
WEST_HASH_FILE="$WS/.build-local.west-yml.sha256"
new_hash="$(sha256sum "$REPO/config/west.yml" | cut -d' ' -f1)"
old_hash="$(cat "$WEST_HASH_FILE" 2>/dev/null || true)"

info "syncing config/ into $WS/config"
rm -rf "$WS/config"
mkdir -p "$WS/config"
cp -R "$REPO/config/." "$WS/config/"

if (( want_update )) || [[ "$new_hash" != "$old_hash" ]]; then
    info "west update (west.yml changed or --update)"
    ( cd "$WS" && west update )
    printf '%s\n' "$new_hash" > "$WEST_HASH_FILE"
else
    info "west.yml unchanged; skipping west update (use --update to force)"
fi

# --- resolve the build matrix from build.yaml ------------------------------
mapfile -t entries < <("$PY" - "$REPO/build.yaml" <<'PY'
import sys
import yaml

# Fields are joined with '|' and no field may contain it. Do NOT use a tab or
# a control character here: bash treats TAB as IFS *whitespace*, so consecutive
# tabs collapse and the empty shield/snippet fields of some entries would shift
# the remaining fields along, and IFS=$'\x1f' does not split at all.
SEP = "|"

with open(sys.argv[1], encoding="utf-8") as f:
    matrix = yaml.safe_load(f)

for e in matrix["include"]:
    fields = [
        str(e["artifact-name"]),
        str(e["board"]),
        str(e.get("shield") or ""),
        str(e.get("snippet") or ""),
        " ".join(str(e.get("cmake-args") or "").split()),
    ]
    for field in fields:
        if SEP in field or "\n" in field:
            raise SystemExit(f"build.yaml value cannot contain {SEP!r}: {field!r}")
    print(SEP.join(fields))
PY
)
(( ${#entries[@]} )) || die "build.yaml produced no entries"

is_selected() {
    local name="$1"
    (( ${#selected[@]} == 0 )) && return 0
    local s
    for s in "${selected[@]}"; do [[ "$s" == "$name" ]] && return 0; done
    return 1
}

mkdir -p "$OUT"
failures=()
built=()

for entry in "${entries[@]}"; do
    IFS='|' read -r name board shield snippet cmake_args <<<"$entry"
    [[ -n "$name" && -n "$board" ]] || die "malformed build.yaml entry: [$entry]"
    is_selected "$name" || continue

    info "building $name  (${shield:+$shield / }$board)"

    d="$WS/build/$name"
    argv=(west build -s zmk/app -d "$d" -b "$board")
    [[ -n "$snippet" ]] && argv+=(-S "$snippet")
    argv+=(-- "-DZMK_CONFIG=$WS/config")
    [[ -n "$shield" ]] && argv+=("-DSHIELD=$shield")
    argv+=("-DZMK_EXTRA_MODULES=$REPO")
    if [[ -n "$cmake_args" ]]; then
        read -r -a extra <<<"$cmake_args"
        argv+=("${extra[@]}")
    fi

    if ! ( cd "$WS" && "${argv[@]}" ); then
        printf '\033[31m!!! %s failed\033[0m\n' "$name" >&2
        failures+=("$name")
        continue
    fi

    if [[ -f "$d/zephyr/zmk.uf2" ]]; then
        cp -f "$d/zephyr/zmk.uf2" "$OUT/$name.uf2"
        built+=("$name.uf2")
    elif [[ -f "$d/zephyr/zmk.bin" ]]; then
        cp -f "$d/zephyr/zmk.bin" "$OUT/$name.bin"
        built+=("$name.bin")
    else
        printf '\033[31m!!! %s produced no zmk.uf2/zmk.bin\033[0m\n' "$name" >&2
        failures+=("$name")
    fi
done

# --- summary ---------------------------------------------------------------
echo
if (( ${#built[@]} )); then
    info "artefacts in $OUT"
    ( cd "$OUT" && ls -l "${built[@]}" )
fi

if (( ${#failures[@]} )); then
    printf '\033[31mfailed: %s\033[0m\n' "${failures[*]}" >&2
    exit 1
fi

info "done"
