#!/bin/sh
# tools/mkmusic.sh -- something for the player to play, generated not shipped.
#
# A music player with an empty library cannot be judged, and checking a real
# song into an OS repository is somebody's copyright. So the sample library is
# synthesised: an arpeggio, which is more useful than a sine because it has
# note ONSETS -- transients are what make the encoder emit short blocks, and
# (the commas inside the expression are escaped: ffmpeg's filter syntax uses
# a comma to separate filters, so an unescaped mod(t,0.5) is a parse error)
# short blocks exercise the window switching, the reordering and the mixed-block
# path that a steady tone never reaches.
set -e
OUT=data/music
mkdir -p "$OUT"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "  (ffmpeg not installed -- no sample music; apt install ffmpeg)"
    exit 0
fi

# An A-major arpeggio, each note plucked so it has an attack and a decay.
if [ ! -f "$OUT/arpeggio.mp3" ]; then
    ffmpeg -hide_banner -loglevel error -f lavfi \
      -i "aevalsrc=exprs=exp(-3*mod(t\,0.5))*0.4*(sin(2*PI*440*t)+0.5*sin(2*PI*554.37*t)+0.3*sin(2*PI*659.25*t)):d=12:s=44100" \
      -ac 2 -b:a 128k "$OUT/arpeggio.mp3" -y
    echo "  generated $OUT/arpeggio.mp3"
fi

# A two-tone chime at a different sample rate, so the player's resampler and
# its 48000-direct path are BOTH exercised by the shipped library.
if [ ! -f "$OUT/chime.mp3" ]; then
    ffmpeg -hide_banner -loglevel error -f lavfi \
      -i "aevalsrc=exprs=exp(-2*mod(t\,1.5))*0.4*(sin(2*PI*880*t)+0.4*sin(2*PI*1318.5*t)):d=9:s=48000" \
      -ac 2 -b:a 192k "$OUT/chime.mp3" -y
    echo "  generated $OUT/chime.mp3"
fi
