#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Makes the test media used on the TV (docs/TV-TEST-PLAN.md) into OUT_DIR, on the desktop:
#   tools/test-media/make.sh OUT_DIR
# Needs ffmpeg (with libx264, libx265, libmp3lame), python3 with Pillow and the DejaVu fonts; the Dolby Vision
# file also needs cargo and the dolby_vision crate in the local cargo registry (skipped otherwise).
# Copy OUT_DIR to the USB drive (e.g. as wnk-test/subs) with ares-push or by hand.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:?usage: make.sh OUT_DIR}"
mkdir -p "$out"
cd "$out"
q=(-loglevel error -y)
font=/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf

# A 1080p25 test picture with a 440 Hz tone, the base of most files.
ffmpeg "${q[@]}" -f lavfi -i testsrc2=size=1920x1080:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -t 45 -c:v libx264 -preset veryfast -g 50 -pix_fmt yuv420p -c:a aac -b:a 128k base.mp4

cat > en.srt <<'SRT'
1
00:00:01,000 --> 00:00:05,000
<i>SubRip line one</i>
second line — café

2
00:00:06,000 --> 00:00:12,000
{\an8}Top? no: plain text at the bottom

3
00:00:13,000 --> 00:00:40,000
The third SubRip cue (long)
SRT
python3 -I - <<'PY'
def ts(ms): return "%02d:%02d:%02d,%03d" % (ms // 3600000, ms // 60000 % 60, ms // 1000 % 60, ms % 1000)
open("sync.srt", "w").write("\n".join(f"{i+1}\n{ts(ms)} --> {ts(ms+200)}\ncue {ms/1000:.1f} s\n"
                                      for i, ms in enumerate(range(0, 30000, 200))))
PY
cat > styled.ass <<'ASS'
[Script Info]
ScriptType: v4.00+
PlayResX: 1920
PlayResY: 1080

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Inter,64,&H0000FFFF,&H000000FF,&H00000000,&H80000000,-1,0,0,0,100,100,0,0,1,4,2,2,60,60,60,1
Style: Top,Inter,56,&H00008AFF,&H000000FF,&H00000000,&H80000000,-1,-1,0,0,100,100,0,0,1,3,1,8,60,60,60,1
Style: Serif,DejaVu Serif,72,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,-1,0,0,0,100,100,0,0,1,4,2,5,60,60,60,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:40.00,Default,,0,0,0,,Styled ASS: yellow, bottom
Dialogue: 0,0:00:01.00,0:00:40.00,Top,,0,0,0,,{\i1}Second style, top, orange{\i0}
Dialogue: 0,0:00:01.00,0:00:40.00,Serif,,0,0,0,,Embedded serif font, centre
ASS
printf 'WEBVTT\n\n00:00:01.000 --> 00:00:40.000\n<v Narrateur>WebVTT : bonjour &amp; bienvenue</v>\n' > fr.vtt

ffmpeg "${q[@]}" -i base.mp4 -i en.srt -i styled.ass -map 0 -map 1 -map 2 -c copy -c:s:0 srt -c:s:1 ass \
    -metadata:s:s:0 language=eng -metadata:s:s:0 title="English SRT" -metadata:s:s:1 language=eng \
    -metadata:s:s:1 title="Styled ASS" -disposition:s:0 default subs-srt-ass.mkv
ffmpeg "${q[@]}" -i base.mp4 -i styled.ass -attach "$font" -metadata:s:t mimetype=application/x-truetype-font \
    -map 0 -map 1 -c copy -c:s ass -metadata:s:s:0 title="ASS with its font" ass-font.mkv
ffmpeg "${q[@]}" -i base.mp4 -i en.srt -map 0 -map 1 -c copy -c:s mov_text -metadata:s:s:0 language=eng subs-tx3g.mp4
ffmpeg "${q[@]}" -i base.mp4 -i fr.vtt -map 0 -map 1 -c copy -c:s webvtt -metadata:s:s:0 language=fre subs-webvtt.mkv
ffmpeg "${q[@]}" -i base.mp4 -i sync.srt -t 30 -map 0 -map 1 -c copy -c:s srt sync.mkv
cp base.mp4 "side car.mp4" && cp en.srt "side car.srt"

# Bitmap subtitles: PGS made here, DVD and DVB encoded from it by FFmpeg.
python3 -I "$here/make_sup.py" labels.sup
ffmpeg "${q[@]}" -i base.mp4 -i labels.sup -t 42 -map 0 -map 1 -map 1 -c:v copy -c:a copy -c:s:0 copy -c:s:1 dvdsub \
    -metadata:s:s:0 title="Blu-ray PGS" -metadata:s:s:1 title="DVD VobSub" -disposition:s:0 default pgs-dvd.mkv
ffmpeg "${q[@]}" -i base.mp4 -i labels.sup -t 42 -map 0:v -map 0:a -map 1 -c:v copy -c:a copy -c:s dvbsub dvb.ts

# Two audio tracks (AAC stereo, AC-3 5.1) and chapters, 90 s.
ffmpeg "${q[@]}" -f lavfi -i testsrc2=size=1920x1080:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -f lavfi -i sine=frequency=880:sample_rate=48000 -t 90 -map 0 -map 1 -map 2 -c:v libx264 -preset veryfast -g 50 \
    -pix_fmt yuv420p -c:a:0 aac -b:a:0 128k -c:a:1 ac3 -ac:a:1 6 -b:a:1 384k -metadata:s:a:0 language=eng \
    -metadata:s:a:0 title="English AAC" -metadata:s:a:1 language=fre -metadata:s:a:1 title="French AC-3 5.1" two-audio.mkv
printf ';FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=30000\ntitle=Opening\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=30000\nEND=60000\ntitle=The middle part\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=60000\nEND=90000\ntitle=Ending\n' > chapters.txt
ffmpeg "${q[@]}" -i two-audio.mkv -i chapters.txt -map 0 -map_metadata 1 -map_chapters 1 -c copy chapters.mkv

# Full-range H.264 (as phones record): the software path's J420 case.
ffmpeg "${q[@]}" -f lavfi -i testsrc2=size=1280x720:rate=30 -t 30 -c:v libx264 -preset veryfast -pix_fmt yuvj420p \
    -color_range pc full-range.mp4

# Teletext subtitles (page 888, German characters) in a TS: FFmpeg cannot encode teletext.
ffmpeg "${q[@]}" -f lavfi -i testsrc2=size=1280x720:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 \
    -t 25 -c:v libx264 -preset veryfast -g 50 -pix_fmt yuv420p -c:a mp2 plain-tt.ts
python3 -I "$here/add_teletext.py" plain-tt.ts teletext.ts

# Closed captions (CEA-608 in H.264 SEI, as US recordings carry them): FFmpeg cannot write them.
ffmpeg "${q[@]}" -f lavfi -i testsrc2=size=1280x720:rate=25 -t 30 -c:v libx264 -preset veryfast -bf 0 -g 50 \
    -pix_fmt yuv420p -x264-params aud=1 -f h264 plain-cc.h264
python3 -I "$here/inject_cc.py" plain-cc.h264 25 cc.h264
ffmpeg "${q[@]}" -r 25 -i cc.h264 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 30 -map 0 -map 1 -c:v copy \
    -c:a aac captions.mkv

# Music laid out Artist/Album: tagged MP3s whose file names are not in track order, a cover; and
# untagged FLACs by a second artist (the Artists view needs two).
album="Music/Test Artist/Tagged Album"
mkdir -p "$album" "Music/Other Artist/Untagged"
tag=("c.mp3:Track One:1:330" "a.mp3:Track Two:2:440" "b.mp3:Track Three:3:550")
for t in "${tag[@]}"; do
    IFS=: read -r name title n hz <<<"$t"
    ffmpeg "${q[@]}" -f lavfi -i sine=frequency=$hz:sample_rate=44100 -t 20 -c:a libmp3lame -b:a 128k \
        -metadata title="$title" -metadata artist="Test Artist" -metadata album="Tagged Album" -metadata track="$n/3" \
        "$album/$name"
done
ffmpeg "${q[@]}" -f lavfi -i testsrc2=size=600x600 -frames:v 1 "$album/cover.jpg"
for n in 1 2; do
    ffmpeg "${q[@]}" -f lavfi -i sine=frequency=$((300 * n)):sample_rate=44100 -t 15 -map_metadata -1 -c:a flac \
        "Music/Other Artist/Untagged/0$n part $n.flac"
done

# Playlists.
printf '#EXTM3U\n#EXTINF:-1 group-title="Local",Two audio tracks\ntwo-audio.mkv\n#EXTINF:-1 group-title="Local",Chapters\nchapters.mkv\n' > local.m3u
printf '[playlist]\nFile1=sync.mkv\nTitle1=Sync check\nFile2=side car.mp4\n' > local.pls

# Dolby Vision Profile 8.1 without a configuration record (the RPU scan's case).
if command -v cargo >/dev/null && cargo build --offline --release --manifest-path "$here/rpugen/Cargo.toml" -q 2>/dev/null; then
    ffmpeg "${q[@]}" -f lavfi -i testsrc2=size=1920x1080:rate=25 -t 30 -pix_fmt yuv420p10le -c:v libx265 -preset ultrafast \
        -x265-params "log-level=error:aud=1:repeat-headers=1:keyint=50:bframes=0:hdr10=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,50):max-cll=1000,400" \
        -f hevc hdr10.hevc
    "$here/rpugen/target/release/rpugen" 750 rpu.bin
    python3 -I "$here/inject_rpu.py" hdr10.hevc rpu.bin dv81.hevc
    ffmpeg "${q[@]}" -r 25 -i dv81.hevc -i base.mp4 -map 0:v -map 1:a -t 30 -c copy dv81.mkv
    rm -f hdr10.hevc rpu.bin dv81.hevc
else
    echo "dv81.mkv skipped (cargo or the dolby_vision crate not available offline)"
fi
# A 4:3 frame (1440×1080) holding a 16:9 picture between black bars, for Crop: Fit boxes it on four
# sides, Crop 16:9 fills the screen.
ffmpeg "${q[@]}" -f lavfi -i testsrc2=size=1440x810:rate=25 -f lavfi -i sine=frequency=330:sample_rate=48000 -t 300 \
    -vf "pad=1440:1080:0:135:black,drawbox=x=0:y=135:w=1440:h=810:color=white:t=4" \
    -c:v libx264 -preset veryfast -g 50 -pix_fmt yuv420p -c:a aac -b:a 96k letterbox-4x3.mp4

rm -f en.srt sync.srt styled.ass fr.vtt labels.sup chapters.txt plain-tt.ts plain-cc.h264 cc.h264
ls -la; ls -R Music
