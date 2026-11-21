#!/usr/bin/env python3
"""Fetch pinned CrossPoint SD fonts for the Q2 reader, with exact SHA-256 verification.

Uses the original CrossPoint project's cpfont v4 release. Never changes
firmware/system fonts or embeds a different Q2-native rendering engine.
"""
from __future__ import annotations
import hashlib
import pathlib
import sys
import urllib.request
from crosspoint_script_fonts import RELEASE as SCRIPT_RELEASE, SCRIPT_FONTS

RELEASE = "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/sd-fonts-m1-b4"
SHA256 = {
    "NotoSansExtended_12.cpfont": "17e2dd57a2507203dcb04e1ce4f7f49c755cd327550c8c8333ac713655b06bd1",
    "NotoSansExtended_14.cpfont": "3c7eabccb4ed9b4ab909c5a3d973dc6bdc19bfbe061bff54643ca4cf859aa116",
    "NotoSansExtended_16.cpfont": "5245afdd81e86b2b9827785b379ac2380ff6dde3bbe15b9ddaba666e8b738967",
    "NotoSansExtended_18.cpfont": "1c0166009a88a567a69d3caadb3a04e2ec2bd91b49b80100211a749e1d02801d",
    "NotoSerifExtended_12.cpfont": "90cbcf43bc504f1ca3dad068e9286d7818dec6522fa2c9810b0e421637944091",
    "NotoSerifExtended_14.cpfont": "d09be89359a201a0b43bcc05abca305e5358a939294f5d38852c82722cbcdb24",
    "NotoSerifExtended_16.cpfont": "c269f21a040173777e2b9aa3e80b4e802da8edf9f2a8f5dac32aa68d3df4438a",
    "NotoSerifExtended_18.cpfont": "5d35e6f0e5216d442106f9a638ed6c40a08e8ed55408b9fdb2844d234cf73b6d",
}
# Original upstream CrossPoint CJK-JP bitmap font: shared Han ideographs,
# Japanese kana and common punctuation. The upstream release currently does
# not ship a locale-specific SC glyph pack; this fallback restores legibility
# for common Simplified Chinese, but Japanese regional glyph forms may differ.
CJK_RELEASE = "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/sd-fonts-m1-b4-r13"
CJK_SHA256 = {
    12: ("08089c8cd141e38c8a6bbe672215a40da4847c524f097e5200ee89eafcdd80d0", 7714459),
    14: ("e3e5fd4c73077efbb37fed26cd61699f4ae48ef16068029833ce666e45bd294b", 10287882),
    16: ("e578dfa034b2359165c18ffdccf31ddbdddc3239b871f8e820f3035336cca7d6", 12614374),
    18: ("3d775063f151b877ee208abc417024e20cdada161bf75a3d1d7645197676616e", 15767535),
}



def main() -> int:
    if len(sys.argv) != 2:
        print("usage: fetch_q2_fonts.py OUTPUT_DIRECTORY", file=sys.stderr)
        return 2
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for original, expected in SHA256.items():
        family, size = original.split("_")
        short = "sans" if family == "NotoSansExtended" else "serif"
        dest = out / f"{short}-{size}"
        with urllib.request.urlopen(f"{RELEASE}/{original}", timeout=90) as response:
            data = response.read(4_000_000)
        digest = hashlib.sha256(data).hexdigest()
        if digest != expected:
            raise RuntimeError(f"{original}: expected {expected}, got {digest}")
        if len(data) < 200_000:
            raise RuntimeError(f"{original}: implausibly short font")
        dest.with_suffix(".cpfont").write_bytes(data)
        print(f"Verified {original}: {len(data)} bytes")
    # A single CJK face per size is shared by both UI typography families.
    # Keep source hashes fixed: packaging a truncated/changed font must fail.
    for size, (expected, expected_size) in CJK_SHA256.items():
        name = f"NotoSansCJKJP_{size}.cpfont"
        dest = out / f"cjk-{size}.cpfont"
        with urllib.request.urlopen(f"{CJK_RELEASE}/{name}", timeout=120) as response:
            data = response.read(17_000_000)
        digest = hashlib.sha256(data).hexdigest()
        if digest != expected or len(data) != expected_size:
            raise RuntimeError(f"{name}: digest/size mismatch: {digest}, {len(data)}")
        dest.write_bytes(data)
        print(f"Verified {name}: {len(data)} bytes")
    # All script families published by the upstream CrossPoint release:
    # Hangul, Devanagari, Bengali, Gujarati, Gurmukhi, Kannada,
    # Malayalam, Odia, Sinhala, Tamil, Telugu, Arabic and Hebrew.
    # Disk-resident .cpfont files: only coverage intervals/needed glyphs
    # are loaded by the Q2 reader, not the full pixel data for all fonts.
    for slug, (family, description, sizes) in SCRIPT_FONTS.items():
        for point_size in (12, 14, 16, 18):
            expected, expected_size = sizes[str(point_size)]
            source = f"{SCRIPT_RELEASE}/{family}_{point_size}.cpfont"
            dest = out / f"{slug}-{point_size}.cpfont"
            with urllib.request.urlopen(source, timeout=120) as response:
                data = response.read(expected_size + 1)
            digest = hashlib.sha256(data).hexdigest()
            if digest != expected or len(data) != expected_size:
                raise RuntimeError(
                    f"{family}_{point_size}: digest/size mismatch {digest} ({len(data)})")
            dest.write_bytes(data)
            print(f"Verified {slug} {point_size}pt ({description}): {len(data)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
