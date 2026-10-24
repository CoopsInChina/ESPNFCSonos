#!/usr/bin/env python3
"""Regenerates the C byte-array sources in this directory from the raw
asset files (boot.jpg, wifi.jpg, ready.jpg, nfc_tap.wav) sitting alongside
this script.

Why C arrays instead of idf_component_register's EMBED_FILES: EMBED_FILES
generates mangled/doubled intermediate .S-file paths under PlatformIO's
ESP-IDF integration (it assumes idf.py's native directory layout) — see
README.md's "Asset pipeline" section. Plain C sources have no such issue
and are picked up by src/CMakeLists.txt's normal *.c glob like any other
source file.

Run from anywhere; paths are relative to this script's own location:
    python3 src/assets/generate.py
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

# (source file, output basename, C identifier prefix)
ASSETS = [
    ("boot.jpg",    "boot_jpg",    "boot_jpg"),
    ("wifi.jpg",    "wifi_jpg",    "wifi_jpg"),
    ("ready.jpg",   "ready_jpg",   "ready_jpg"),
    ("nfc_tap.wav", "nfc_tap_wav", "nfc_tap_wav"),
]

# The converted clip's peak was ~85% of full scale (measured directly).
# First pass matched the beep tone's ~27% (9000 of 32767); second pass
# (4000) was still too loud in practice. Halved again.
TAP_SOUND_TARGET_PEAK = 2000


def scale_wav_in_place(data: bytearray, target_peak: int) -> None:
    """Finds the WAV's "data" sub-chunk and scales its int16 PCM samples
    so their peak matches target_peak — same RIFF-walk buzzer.c does at
    runtime, so this only ever touches the actual sample bytes, never the
    chunk sizes."""
    pos = 12   # past the 12-byte RIFF/WAVE header
    data_off = data_len = None
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        chunk_len = struct.unpack_from("<I", data, pos + 4)[0]
        if chunk_id == b"data":
            data_off, data_len = pos + 8, chunk_len
            break
        pos += 8 + chunk_len + (chunk_len & 1)
    if data_off is None:
        raise RuntimeError("no data chunk found in WAV")

    n_samples = data_len // 2
    samples = list(struct.unpack_from(f"<{n_samples}h", data, data_off))
    peak = max(abs(s) for s in samples) or 1
    gain = target_peak / peak
    scaled = [max(-32768, min(32767, int(s * gain))) for s in samples]
    struct.pack_into(f"<{n_samples}h", data, data_off, *scaled)
    print(f"  scaled WAV peak {peak} -> {target_peak} (gain {gain:.3f})")


def generate(src_name: str, out_name: str, var: str) -> None:
    src_path = os.path.join(HERE, src_name)
    with open(src_path, "rb") as f:
        data = bytearray(f.read())

    if src_name.endswith(".wav"):
        scale_wav_in_place(data, TAP_SOUND_TARGET_PEAK)

    c_path = os.path.join(HERE, f"{out_name}.c")
    with open(c_path, "w") as f:
        f.write("#include <stdint.h>\n#include <stddef.h>\n\n")
        f.write(f"const uint8_t {var}_data[] = {{\n")
        for i in range(0, len(data), 20):
            chunk = data[i:i + 20]
            f.write("    " + ",".join(str(b) for b in chunk) + ",\n")
        f.write("};\n")
        f.write(f"const size_t {var}_len = sizeof({var}_data);\n")

    h_path = os.path.join(HERE, f"{out_name}.h")
    with open(h_path, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n")
        f.write(f"extern const uint8_t {var}_data[];\n")
        f.write(f"extern const size_t {var}_len;\n")

    print(f"{out_name}.c: {len(data)} source bytes -> {os.path.getsize(c_path)} bytes of generated C")


if __name__ == "__main__":
    for src_name, out_name, var in ASSETS:
        generate(src_name, out_name, var)
