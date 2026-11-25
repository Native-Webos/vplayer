#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Makes a PAL DVD image with a menu, for the disc menu tests (docs/TV-TEST-PLAN.md):
#   tools/test-media/make_dvd.sh OUT_DIR      -> OUT_DIR/test-dvd.iso
# The disc starts at its menu: two buttons, "Main film" (title 1, 30 s, chapters at 10 s and 20 s)
# and "Extras" (title 2, 12 s); each title returns to the menu at its end.
# Needs ffmpeg, python3 with Pillow, the DejaVu fonts and docker (dvdauthor, spumux and genisoimage
# run in a Debian container).
set -euo pipefail
out="${1:?usage: make_dvd.sh OUT_DIR}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
work="$out/dvd-work"
rm -rf "$work"
mkdir -p "$work"
cd "$work"
q=(-loglevel error -y)
font=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf

# The menu's picture and its button highlights (spumux: transparent, at most 3 colours).
python3 - "$font" <<'PY'
import sys
from PIL import Image, ImageDraw, ImageFont
font = sys.argv[1]
big, mid = ImageFont.truetype(font, 56), ImageFont.truetype(font, 40)
bg = Image.new('RGB', (720, 576), (20, 30, 60))
d = ImageDraw.Draw(bg)
d.text((360, 120), 'VPLAYER TEST DVD', font=big, fill=(255, 255, 255), anchor='mm')
d.text((360, 300), 'Main film', font=mid, fill=(230, 230, 230), anchor='mm')
d.text((360, 400), 'Extras', font=mid, fill=(230, 230, 230), anchor='mm')
bg.save('menu_bg.png')
for name, colour in (('highlight.png', (255, 140, 0, 255)), ('select.png', (255, 255, 0, 255))):
    im = Image.new('RGBA', (720, 576), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for y in (300, 400):
        d.rectangle((200, y - 36, 520, y + 36), outline=colour, width=6)
    im.save(name)
PY

# The menu: a still picture with silence; the titles: test pictures with their names and a clock.
ffmpeg "${q[@]}" -loop 1 -i menu_bg.png -f lavfi -i anullsrc=r=48000:cl=stereo -t 4 \
    -target pal-dvd -aspect 4:3 menu.mpg
title() {  # NAME SECONDS TEXT
    ffmpeg "${q[@]}" -f lavfi -i "testsrc=size=720x576:rate=25" -f lavfi -i "sine=frequency=$4:sample_rate=48000" -t "$2" \
        -vf "drawtext=fontfile=$font:text='$3':fontsize=56:fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=60,drawtext=fontfile=$font:text='%{pts\:hms}':fontsize=48:fontcolor=yellow:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=h-120" \
        -target pal-dvd -aspect 16:9 "$1.mpg"
}
title film 30 "MAIN FILM (title 1)" 440
title extras 12 "EXTRAS (title 2)" 660

cat > menu.xml <<'XML'
<subpictures format="PAL">
  <stream>
    <spu force="yes" start="00:00:00.00" highlight="highlight.png" select="select.png">
      <button name="film" x0="200" y0="264" x1="520" y1="336" down="extras"/>
      <button name="extras" x0="200" y0="364" x1="520" y1="436" up="film"/>
    </spu>
  </stream>
</subpictures>
XML
cat > dvd.xml <<'XML'
<dvdauthor dest="dvd">
  <vmgm>
    <fpc>jump titleset 1 menu;</fpc>
  </vmgm>
  <titleset>
    <menus>
      <video format="pal" aspect="4:3"/>
      <pgc entry="root">
        <button name="film">jump title 1;</button>
        <button name="extras">jump title 2;</button>
        <vob file="menu_buttons.mpg" pause="inf"/>
      </pgc>
    </menus>
    <titles>
      <video format="pal" aspect="16:9"/>
      <pgc>
        <vob file="film.mpg" chapters="0,0:10,0:20"/>
        <post>call menu;</post>
      </pgc>
      <pgc>
        <vob file="extras.mpg"/>
        <post>call menu;</post>
      </pgc>
    </titles>
  </titleset>
</dvdauthor>
XML

docker run --rm -v "$work:/w" -w /w -u "$(id -u):$(id -g)" -e VIDEO_FORMAT=PAL "$(docker build -q - <<'DOCKERFILE'
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends dvdauthor genisoimage && rm -rf /var/lib/apt/lists/*
DOCKERFILE
)" sh -c 'spumux -m dvd menu.xml < menu.mpg > menu_buttons.mpg && dvdauthor -x dvd.xml &&
          genisoimage -quiet -dvd-video -V VPLAYER_TEST -o test-dvd.iso dvd' 2>&1 | grep -iE "error|warn" || true
mv test-dvd.iso "$out/"
cd "$out"
rm -rf "$work"
ls -la "$out/test-dvd.iso"
