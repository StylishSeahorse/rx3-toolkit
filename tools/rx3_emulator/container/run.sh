#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
set -eu
ulimit -c 0

PROFILE=${RX3EMU_PROFILE:-all}
DURATION=${RX3EMU_DURATION:-60}
MEDIA=${RX3EMU_MEDIA:-}
RBP=/rx3/root/pdj/rbp
OUT=/rx3/tmp/rx3emu
PDJ=/work/pdj
GUI=/work/gui

case "$PROFILE" in
    stock|keyshift|stems|all) ;;
    *) echo "unsupported emulator profile: $PROFILE" >&2; exit 64 ;;
esac

mkdir -p /work "$PDJ" "$GUI" /rx3/tmp "$OUT" /rx3/media/usb/RX3_STEMS
mount --bind /output "$OUT"
mount -t proc proc /rx3/proc
# JUCE's RX3-specific ALSA backend selects its device table from the board
# revision in /proc/cpuinfo. QEMU-user exposes the host CPU instead, so provide
# the same 0x700 identity that the firmware expects on the rev-7 audio board.
printf 'Processor\t: ARMv7 Processor rev 10 (v7l)\nRevision\t: 00000700\n' \
    > /work/rx3-cpuinfo
mount --bind /work/rx3-cpuinfo /rx3/proc/cpuinfo
mount --rbind /dev /rx3/dev
if [ -n "$MEDIA" ]; then
    mkdir -p /rx3/media/usb1
    mount --bind "$MEDIA" /rx3/media/usb1
fi

# rbp and its fixed-path assets live in a private copy. The laboratory sysroot
# remains mounted read-only on the host.
cp -a /rx3/root/pdj/. "$PDJ"/
mount --bind "$PDJ" /rx3/root/pdj
cp -a /rx3/root/gui/. "$GUI"/
mount --bind "$GUI" /rx3/root/gui
cp /opt/rx3emu/fbshim.so /rx3/root/pdj/rx3emu-fbshim.so
cp /opt/rx3emu/audioshim.so /rx3/root/pdj/rx3emu-audioshim.so

PRELOAD=/root/pdj/rx3emu-fbshim.so:/root/pdj/rx3emu-audioshim.so
if [ "$PROFILE" != stock ]; then
    test -r /repo/build/librx3_core_emulator.so
    cp /repo/build/librx3_core_emulator.so /rx3/root/pdj/librx3_core.so
    cp /repo/mod/modules/core/1.19/assets/key-selected.rgb565 \
       /rx3/root/pdj/rx3-key-selected.rgb565
    cp /repo/mod/modules/core/1.19/assets/stems-selected.rgb565 \
       /rx3/root/pdj/rx3-stems-selected.rgb565
    cp /repo/mod/modules/core/1.19/assets/none-selected.rgb565 \
       /rx3/root/pdj/rx3-none-selected.rgb565
    cp /repo/mod/modules/core/1.19/assets/status-none-selected.rgb565 \
       /rx3/root/pdj/rx3-status-none-selected.rgb565

    FOUND=$(dd if="$RBP" bs=1 skip=1874220 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
    case "$FOUND" in
        cc3501e3)
            printf '\003\066\001\343' | dd of="$RBP" bs=1 seek=1874220 conv=notrunc 2>/dev/null
            ;;
        033601e3) ;;
        *) echo "image-table guard rejected: $FOUND" >&2; exit 65 ;;
    esac
    PRELOAD="$PRELOAD:/root/pdj/librx3_core.so"
fi

case "$PROFILE" in
    keyshift|all) KEYSHIFT=1 ;;
    *) KEYSHIFT=0 ;;
esac
case "$PROFILE" in
    stems|all) STEMS=/media/usb/RX3_STEMS ;;
    *) STEMS= ;;
esac
case "$PROFILE" in
    keyshift|all) PANEL=1 ;;
    stems) PANEL=2 ;;
    *) PANEL=0 ;;
esac

rm -f "$OUT/framebuffer.raw" "$OUT/framebuffer.json" \
      "$OUT/rbp.log" "$OUT/hook.log" "$OUT/hardware.log" \
      "$OUT/crash.log" "$OUT/audio.log" \
      "$OUT/audio-playback-0.json" "$OUT/audio-playback-0.raw" \
      "$OUT/audio-playback-1.json" "$OUT/audio-playback-1.raw" \
      "$OUT/audio-playback-2.json" "$OUT/audio-playback-2.raw" \
      "$OUT/ready" "$OUT/status"
rm -f "$OUT/touch.fifo" "$OUT/touch.command"
printf '0 0 0\n' > "$OUT/touch.command"
if [ -n "$MEDIA" ]; then
    {
        printf '{"mounted":true,"root":"%s","export_pdb":' "$MEDIA"
        [ -f "$MEDIA/PIONEER/rekordbox/export.pdb" ] && printf 'true' || printf 'false'
        printf ',"export_ext_pdb":'
        [ -f "$MEDIA/PIONEER/rekordbox/exportExt.pdb" ] && printf 'true' || printf 'false'
        printf ',"pdtl_db":'
        [ -f "$MEDIA/PIONEER/LIBRARY/PDTL.DB" ] && printf 'true' || printf 'false'
        printf '}\n'
    } > "$OUT/media.json"
else
    printf '{"mounted":false,"export_pdb":false,"export_ext_pdb":false,"pdtl_db":false}\n' > "$OUT/media.json"
fi
rm -f /rx3/tmp/rx3emu-touch.fifo
mkfifo /rx3/tmp/rx3emu-touch.fifo

cleanup()
{
    trap - EXIT INT TERM
    for process in ${RBP_PID:-} ${STDIN_WRITER_PID:-} ${DB_WRITER_PID:-} ${DB_PID:-}; do
        [ -n "$process" ] && kill "$process" 2>/dev/null || true
    done
    [ -n "${RBP_PID:-}" ] && wait "$RBP_PID" 2>/dev/null || true
    cp /rx3/tmp/rx3-stems.log "$OUT/hook.log" 2>/dev/null || true
    cp /rx3/tmp/rx3-performance.ready "$OUT/ready" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

chroot /rx3 /bin/sh -c '/usr/bin/edb_streamd' > "$OUT/edb.log" 2>&1 &
DB_PID=$!
sleep 2

# Keep both FIFOs open without making them perpetually readable. rbp otherwise
# spins on stdin or blocks the LocalDBServer initialization path.
( sleep 100000 > /rx3/tmp/req_LocalDBServer ) &
DB_WRITER_PID=$!
mkfifo /rx3/tmp/stdin.fifo 2>/dev/null || true
( sleep 100000 > /rx3/tmp/stdin.fifo ) &
STDIN_WRITER_PID=$!

chroot /rx3 /bin/sh -c \
    "cd /root/pdj && \
     LD_PRELOAD='$PRELOAD' RX3_KEYSHIFT='$KEYSHIFT' RX3_STEMS_DIR='$STEMS' \
     RX3_EMULATOR_PANEL='$PANEL' \
     RX3_EMULATOR_AUDIO='1' \
     RX3EMU_OUTPUT=/tmp/rx3emu \
     DFBARGS='system=fbdev,no-vt,no-sighandler,no-cursor,no-hardware,disable-module=keyboard,disable-module=linux_input,disable-module=gal,mode=1280x720,depth=32' \
     ./rbp -a < /tmp/stdin.fifo" > "$OUT/rbp.log" 2>&1 &
RBP_PID=$!

echo "running profile=$PROFILE pid=$RBP_PID duration=${DURATION}s"
ELAPSED=0
while kill -0 "$RBP_PID" 2>/dev/null && [ "$ELAPSED" -lt "$DURATION" ]; do
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

if kill -0 "$RBP_PID" 2>/dev/null; then
    printf 'timeout\n' > "$OUT/status"
else
    wait "$RBP_PID" || true
    printf 'rbp-exited\n' > "$OUT/status"
fi
