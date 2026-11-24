#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Screenshots of the player and each tab of its panel, from a host build (demo engine).
#   tools/player-shots.sh OUT_DIR VIDEO_PATH
# VIDEO_PATH should sit in a folder with other videos, so previous/next have a queue (any files with
# video extensions will do: the host build plays a test pattern whatever they contain).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
out="${1:?usage: tools/player-shots.sh OUT_DIR VIDEO_PATH}"
video="${2:?usage: tools/player-shots.sh OUT_DIR VIDEO_PATH}"
mkdir -p "$out"
params="{\"play\":\"$video\"}"

shot() {  # name keys
    local name="${1:?}"
    SDL_VIDEODRIVER=offscreen APP_VPLAYER_KEYS="$2" APP_VPLAYER_SHOT="${out:?}/$name.bmp" \
        timeout 60 build/host-debug/vplayer "$params" >"${out:?}/$name.log" 2>&1 || echo "$name: exit $?"
    python3 -I -c "import sys; from PIL import Image; Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])" \
        "${out:?}/$name.bmp" "${out:?}/$name.png" && rm -f "${out:?}/$name.bmp"
    echo "$name: $(grep -cE ' ERROR ' "${out:?}/$name.log" || true) errors"
}

# Focus starts on play/pause; Right twice reaches Audio, which opens the panel on its first row.
# In the panel, Up reaches the tabs and Right moves between them (and selects).
open="wait@1200,Right,Right,Return,wait@500"
shot 10-bar      "wait@1500,wait@10"
shot 11-audio    "$open,wait@700,wait@10"
shot 12-subs     "$open,Up,Right,wait@700,wait@10"
shot 13-video    "$open,Up,Right,Right,wait@500,Down,Down,Down,Down,Return,wait@700,wait@10"
shot 14-playback "$open,Up,Right,Right,Right,wait@700,wait@10"
shot 15-info     "$open,Up,Right,Right,Right,Right,wait@700,wait@10"
