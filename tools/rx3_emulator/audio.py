"""Convert RX3 ALSA playback captures into ordinary stereo WAV files."""

from __future__ import annotations

import json
import pathlib
import wave


class AudioCaptureError(ValueError):
    pass


def export_wav(raw: pathlib.Path, metadata: pathlib.Path, output: pathlib.Path) -> dict[str, object]:
    details = json.loads(metadata.read_text(encoding="utf-8"))
    rate = int(details["rate"])
    channels = int(details["channels"])
    sample_bytes = int(details["sample_bytes"])
    audio_format = int(details["format"])
    if rate <= 0 or channels <= 0 or sample_bytes != 4 or audio_format != 6:
        raise AudioCaptureError("expected RX3 S24_LE audio in 32-bit ALSA containers")
    payload = raw.read_bytes()
    frame_bytes = channels * sample_bytes
    frames = len(payload) // frame_bytes
    payload = payload[:frames * frame_bytes]
    stereo = bytearray(frames * 6)
    for frame in range(frames):
        source = frame * frame_bytes
        target = frame * 6
        stereo[target:target + 3] = payload[source:source + 3]
        right = source + (4 if channels > 1 else 0)
        stereo[target + 3:target + 6] = payload[right:right + 3]
    with wave.open(str(output), "wb") as destination:
        destination.setnchannels(2)
        destination.setsampwidth(3)
        destination.setframerate(rate)
        destination.writeframes(stereo)
    return {
        "raw": str(raw), "wav": str(output), "rate": rate,
        "source_channels": channels, "channels": 2, "frames": frames,
        "nonzero_bytes": sum(byte != 0 for byte in stereo),
    }


def export_audio_buses(directory: pathlib.Path) -> list[dict[str, object]]:
    buses = []
    for metadata in sorted(directory.glob("audio-playback-*.json")):
        stem = metadata.stem
        raw = directory / f"{stem}.raw"
        if not raw.is_file() or not raw.stat().st_size:
            continue
        try:
            buses.append(export_wav(raw, metadata, directory / f"{stem}.wav"))
        except (OSError, KeyError, TypeError, json.JSONDecodeError, AudioCaptureError):
            continue
    return buses
