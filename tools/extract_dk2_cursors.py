#!/usr/bin/env python3
"""Extract the original DK2 cursor PNGs without committing game assets."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct


CURSOR_LAYOUT = {
    "Point.png": ("idle", 41, (14, 32)),
    "SpellCast.png": ("spell_cast", 12, (5, 65)),
    "SpellPossess.png": ("spell_possess", 6, (2, 2)),
    "DropGold.png": ("drop_gold", 16, (10, 40)),
    "DropThing.png": ("drop_thing", 14, (5, 40)),
    "Slap.png": ("slap", 15, (5, 40)),
    "Idle.png": ("pointer", 1, (2, 14)),
    "PickAxeTag.png": ("pickaxe_tag", 1, (10, 65)),
    "PickAxeHold.png": ("hold_pickaxe", 1, (3, 40)),
    "PickAxeHoldTagging.png": ("hold_pickaxe_tagging", 1, (10, 65)),
    "SpellHold.png": ("hold_spell", 1, (30, 50)),
    "HoldGold.png": ("hold_gold", 1, (5, 5)),
    "HoldThing.png": ("hold_thing", 1, (5, 40)),
    "SpellPossessNoGo.png": ("no_spell_possess", 1, (32, 32)),
    "SmallPoint.png": ("small_point", 1, None),
}


def wad_entries(data: bytes) -> list[tuple[str, bytes]]:
    if len(data) < 0x58 or data[:4] != b"DWFB":
        raise ValueError("not a Dungeon Keeper II WAD")
    if struct.unpack_from("<I", data, 4)[0] != 2:
        raise ValueError("unsupported WAD version")
    count, names_offset, names_size, _ = struct.unpack_from("<4I", data, 0x48)
    table_end = 0x58 + count * 40
    names_end = names_offset + names_size
    if table_end > len(data) or names_end > len(data):
        raise ValueError("truncated WAD index")

    names = data[names_offset:names_end]
    directory = ""
    result: list[tuple[str, bytes]] = []
    for index in range(count):
        fields = struct.unpack_from("<10I", data, 0x58 + index * 40)
        _, name_offset, name_size, offset, stored_size, entry_type, _, _, _, _ = fields
        relative = name_offset - names_offset
        if relative < 0 or relative + name_size > len(names):
            raise ValueError(f"invalid WAD name at entry {index}")
        name = names[relative:relative + name_size].split(b"\0", 1)[0]
        name = name.decode("latin-1").strip().replace("\\", "/")
        if "/" in name:
            directory = name.rsplit("/", 1)[0] + "/"
        elif directory:
            name = directory + name
        if offset + stored_size > len(data):
            raise ValueError(f"truncated WAD entry: {name}")
        if name.lower().endswith(".png"):
            if entry_type != 0:
                raise ValueError(f"compressed cursor entry is unsupported: {name}")
            result.append((Path(name).name, data[offset:offset + stored_size]))
    return result


def png_info(data: bytes) -> tuple[int, int, int, int]:
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("cursor entry is not a PNG")
    width, height, bit_depth, color_type = struct.unpack_from(">IIBB", data, 16)
    return width, height, bit_depth, color_type


def write_file(path: Path, data: bytes, overwrite: bool) -> None:
    if path.exists() and path.read_bytes() != data and not overwrite:
        raise FileExistsError(f"refusing to overwrite {path}; pass --overwrite")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("game", type=Path, help="original Dungeon Keeper 2 directory")
    parser.add_argument("output", type=Path, help="destination for derived cursor assets")
    parser.add_argument("--split-frames", action="store_true",
                        help="also split vertical animation sheets (requires Pillow)")
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    wad = args.game.expanduser() / "Data" / "Sprite.WAD"
    if not wad.is_file():
        raise FileNotFoundError(wad)
    wad_data = wad.read_bytes()
    entries = wad_entries(wad_data)
    found = {name for name, _ in entries}
    missing = sorted(set(CURSOR_LAYOUT) - found)
    if missing:
        raise ValueError(f"Sprite.WAD is missing cursor assets: {', '.join(missing)}")

    output = args.output.expanduser()
    originals = output / "original"
    manifest_assets = []
    for name, data in entries:
        write_file(originals / name, data, args.overwrite)
        width, height, bit_depth, color_type = png_info(data)
        cursor_name, frame_count, hotspot = CURSOR_LAYOUT[name]
        if height % frame_count:
            raise ValueError(f"{name}: height is not divisible by {frame_count} frames")
        manifest_assets.append({
            "cursor": cursor_name,
            "file": name,
            "width": width,
            "height": height,
            "frame_count": frame_count,
            "frame_height": height // frame_count,
            "hotspot": list(hotspot) if hotspot else None,
            "png_bit_depth": bit_depth,
            "png_color_type": color_type,
            "sha256": hashlib.sha256(data).hexdigest(),
        })

    if args.split_frames:
        try:
            from PIL import Image
        except ImportError as error:
            raise RuntimeError("--split-frames requires Pillow") from error
        for asset in manifest_assets:
            image = Image.open(originals / asset["file"])
            frame_height = asset["frame_height"]
            frame_dir = output / "frames" / asset["cursor"]
            for frame in range(asset["frame_count"]):
                crop = image.crop((0, frame * frame_height, image.width,
                                   (frame + 1) * frame_height))
                frame_path = frame_dir / f"{frame:03}.png"
                frame_path.parent.mkdir(parents=True, exist_ok=True)
                if frame_path.exists() and not args.overwrite:
                    raise FileExistsError(f"refusing to overwrite {frame_path}; pass --overwrite")
                crop.save(frame_path, format="PNG", optimize=False)

    manifest = {
        "format": 1,
        "source": "Data/Sprite.WAD",
        "source_sha256": hashlib.sha256(wad_data).hexdigest(),
        "note": "Derived from the user's original DK2 installation; do not redistribute.",
        "assets": manifest_assets,
    }
    manifest_data = (json.dumps(manifest, indent=2) + "\n").encode()
    write_file(output / "manifest.json", manifest_data, args.overwrite)
    print(f"Extracted {len(manifest_assets)} cursor sheets to {output}")


if __name__ == "__main__":
    main()
