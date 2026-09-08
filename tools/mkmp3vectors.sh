#!/bin/sh
# tools/mkmp3vectors.sh -- the MP3 test corpus, made by a real encoder.
#
# Variety, not size. A whole 8490-frame song passed the decoder while two
# 2-second clips failed, because the song's encoder never set SCFSI and theirs
# did -- so what matters here is covering different HABITS: mono and stereo,
# the lowest and highest bitrates, VBR, noise (which forces short blocks and
# the linbits tables), and a sweep (which moves energy across every band).
#
# Regenerate by deleting build/mp3ref and re-running.
set -e
OUT=build/mp3ref
mkdir -p "$OUT"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "  (ffmpeg not installed -- no vectors; apt install ffmpeg)"
    exit 0
fi

gen() {
    out="$OUT/$1"; shift
    [ -f "$out" ] && return 0
    ffmpeg -hide_banner -loglevel error "$@" "$out" -y
    echo "  generated $out"
}

gen tone440.mp3  -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" -ac 2 -b:a 128k
gen mono64.mp3   -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" -ac 1 -b:a 64k
gen noise320.mp3 -f lavfi -i "anoisesrc=d=2:c=pink:r=44100"                    -ac 2 -b:a 320k
gen vbr32k.mp3   -f lavfi -i "sine=frequency=800:sample_rate=32000:duration=2" -ac 2 -q:a 4
gen sweep.mp3    -f lavfi -i "aevalsrc=0.5*sin(1000*t*t):d=2:s=48000"          -ac 2 -b:a 128k
