#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Screenshots of the main screens from a host build, driven by scripted remote keys.
#   tools/host-shots.sh OUT_DIR [SAMPLE_FOLDER]
# SAMPLE_FOLDER is shown as an extra "drive" (VPLAYER_EXTRA_ROOT). Needs ./build.sh first.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
out="${1:?usage: tools/host-shots.sh OUT_DIR [SAMPLE_FOLDER]}"
mkdir -p "$out"
[[ -n "${2:-}" ]] && export VPLAYER_EXTRA_ROOT="$2"

shot() {  # name keys [launch-params-json]
    SDL_VIDEODRIVER=offscreen APP_VPLAYER_KEYS="$2" APP_VPLAYER_SHOT="$out/$1.bmp" \
        timeout 60 build/host-debug/vplayer ${3:+"$3"} >"$out/$1.log" 2>&1 || echo "$1: exit $?"
    python3 -I -c "import sys; from PIL import Image; Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])" \
        "$out/$1.bmp" "$out/$1.png" && rm -f "$out/$1.bmp"
    echo "$1: $(grep -cE ' ERROR ' "$out/$1.log" || true) errors"
}

# A trailing wait@ does not hold the app open by itself (the script ends once no step is pending),
# so every list ends with a tiny wait@10 after the real one.
# Focus starts on the hero card. Up reaches the tabs (Video), Right moves along them and selects.
shot 1-home        "wait@1500,wait@10"
shot 2-home-rail   "wait@1000,Down,wait@700,wait@10"
shot 3-browse      "wait@1000,Up,wait@200,Right,Right,wait@600,Down,wait@700,wait@10"
shot 4-folder      "wait@1000,Up,wait@200,Right,Right,wait@600,Down,wait@500,Return,wait@700,Right,Return,wait@700,Return,wait@900,wait@10"
shot 5-network     "wait@1000,Up,wait@200,Right,Right,Right,wait@800,wait@10"
shot 6-about       "wait@1000,Up,wait@200,Right,Right,Right,Right,Right,Right,wait@300,Return,wait@900,wait@10"
shot 7-player      "wait@1500,wait@10" '{"play":"/demo/Mountain Trip (2025) 2160p.mkv"}'
shot 8-player-seek "wait@1200,Up,wait@300,Right,wait@500,wait@10" '{"play":"/demo/Mountain Trip (2025) 2160p.mkv"}'
shot 9-player-bare "wait@5500,wait@10" '{"play":"/demo/Mountain Trip (2025) 2160p.mkv"}'
