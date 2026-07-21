#!/usr/bin/env python3
"""Batch-upscale dumped DK2 textures through an image-upscaling HTTP server (RealESRGAN x4).

Reads <hash>_<WxH>.png files from the dump directory, writes <hash>.png into
the HD directory (the content hash is the lookup key of the Metal host's
replace mode). Already-present outputs are skipped, so reruns only process
new dumps, and deleting a bad HD file simply falls back to the original
texture in-game.

Alpha is preserved: the RGB and the alpha channel are upscaled separately and
rejoined (SaveImage writes RGBA when the image carries alpha).

Usage:
  python3 tools/upscale_textures.py \
      --src "$HOME/Library/Application Support/Dungeon Keeper II/texture-dump" \
      --dst "$HOME/Library/Application Support/Dungeon Keeper II/textures-hd" \
      --server http://127.0.0.1:8188
"""
import argparse
import json
import pathlib
import re
import sys
import time
import urllib.parse
import urllib.request
import uuid

MODEL = "RealESRGAN_x4plus.safetensors"


def api(server, path, data=None, timeout=30):
    req = urllib.request.Request(server + path)
    if data is not None:
        req.data = json.dumps(data).encode()
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)


def upload(server, path):
    boundary = uuid.uuid4().hex
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="image"; filename="{path.name}"\r\n'
        f"Content-Type: image/png\r\n\r\n"
    ).encode() + path.read_bytes() + f"\r\n--{boundary}\r\n" \
        f'Content-Disposition: form-data; name="overwrite"\r\n\r\ntrue\r\n' \
        f"--{boundary}--\r\n".encode()
    req = urllib.request.Request(server + "/upload/image", data=body)
    req.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)["name"]


def workflow(image_name, prefix):
    return {
        "1": {"class_type": "LoadImage", "inputs": {"image": image_name}},
        "2": {"class_type": "UpscaleModelLoader", "inputs": {"model_name": MODEL}},
        "3": {"class_type": "ImageUpscaleWithModel",
              "inputs": {"upscale_model": ["2", 0], "image": ["1", 0]}},
        # upscale the alpha mask with the same model and rejoin
        "4": {"class_type": "MaskToImage", "inputs": {"mask": ["1", 1]}},
        "5": {"class_type": "ImageUpscaleWithModel",
              "inputs": {"upscale_model": ["2", 0], "image": ["4", 0]}},
        "6": {"class_type": "ImageToMask", "inputs": {"image": ["5", 0], "channel": "red"}},
        "7": {"class_type": "JoinImageWithAlpha",
              "inputs": {"image": ["3", 0], "alpha": ["6", 0]}},
        "8": {"class_type": "SaveImage",
              "inputs": {"images": ["7", 0], "filename_prefix": prefix}},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, type=pathlib.Path)
    ap.add_argument("--dst", required=True, type=pathlib.Path)
    ap.add_argument("--server", default="http://127.0.0.1:8188")
    args = ap.parse_args()
    args.dst.mkdir(parents=True, exist_ok=True)

    # Two input layouts: legacy host dumps (<hash>_<WxH>.png -> <hash>.png)
    # and named full-library dumps (<name>MM0.png -> <name>.png; only the
    # MM0 base mip is upscaled, lower mips are the host's job).
    hash_re = re.compile(r"^([0-9a-f]{16})_\d+x\d+\.png$")
    mm0_re = re.compile(r"^(.+)MM0\.png$")
    todo = []
    for path in sorted(args.src.iterdir()):
        m = hash_re.match(path.name) or mm0_re.match(path.name)
        if not m:
            continue
        out = args.dst / f"{m.group(1)}.png"
        if not out.exists():
            todo.append((path, out))
    print(f"{len(todo)} textures to upscale")

    done = failed = 0
    for path, out in todo:
        try:
            uploaded = upload(args.server, path)
            prefix = f"dk2hd/{out.stem}"
            pid = api(args.server, "/prompt", {"prompt": workflow(uploaded, prefix)})["prompt_id"]
            for _ in range(120):
                time.sleep(0.5)
                h = api(args.server, f"/history/{pid}")
                if pid in h:
                    break
            else:
                raise RuntimeError("timeout")
            entry = h[pid]
            if entry["status"]["status_str"] != "success":
                raise RuntimeError(entry["status"]["status_str"])
            img = next(i for o in entry["outputs"].values() for i in o.get("images", []))
            query = urllib.parse.urlencode(
                {"filename": img["filename"], "subfolder": img["subfolder"], "type": img["type"]})
            with urllib.request.urlopen(f"{args.server}/view?{query}", timeout=60) as r:
                out.write_bytes(r.read())
            done += 1
            print(f"[{done}/{len(todo)}] {out.name}")
        except Exception as e:  # keep going; rerun picks up the rest
            failed += 1
            print(f"FAILED {path.name}: {e}", file=sys.stderr)
    print(f"done: {done}, failed: {failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
