#!/usr/bin/env python3
"""LZ4-compress the generated image sources in main/ui/.

EEZ Studio's export writes every image uncompressed (6.5 MiB of the 7.2 MiB app).
This regenerates each main/ui/ui_image_<name>.c from GUI/ASSETS/art/<name>.png
with LVGL's own converter (--compress LZ4), keeping the colour format EEZ chose
and the img_<name> symbol the generated screens use. Run it after EVERY EEZ
export, because the export overwrites these files.

Setup (once):
    python3 -m venv /tmp/imgenv && /tmp/imgenv/bin/pip install pypng lz4 pillow
Run (from this folder):
    /tmp/imgenv/bin/python tools/compress_images.py

Needs CONFIG_LV_USE_LZ4 and CONFIG_LV_BIN_DECODER_RAM_LOAD (sdkconfig.defaults)
and an image cache (CONFIG_LV_CACHE_DEF_SIZE), or every redraw decodes again.
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "main" / "ui"
ART = ROOT / "GUI" / "ASSETS" / "art"
CONVERTER = ROOT / "managed_components" / "lvgl__lvgl" / "scripts" / "LVGLImage.py"


def main() -> int:
    if not CONVERTER.exists():
        print(f"missing {CONVERTER}: run `idf.py reconfigure` first", file=sys.stderr)
        return 1
    before = after = 0
    for src in sorted(UI.glob("ui_image_*.c")):
        name = src.stem[len("ui_image_"):]
        png = ART / f"{name}.png"
        text = src.read_text()
        cf = re.search(r"\.(?:header\.)?cf = LV_COLOR_FORMAT_(\w+)", text)
        if not png.exists() or not cf:
            print(f"skip {src.name}: no {png.name} or no colour format")
            continue
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(
                [sys.executable, str(CONVERTER), "--ofmt", "C", "--cf", cf.group(1),
                 "--compress", "LZ4", "-o", tmp, str(png)],
                check=True, stdout=subprocess.DEVNULL)
            out = (Path(tmp) / f"{name}.c").read_text()
        out = re.sub(rf"\b{name}_map\b", f"img_{name}_map", out)
        out = re.sub(rf"(lv_image_dsc_t\s+){name}\b", rf"\1img_{name}", out)
        out = out.replace('"lvgl/lvgl.h"', '"lvgl.h"')
        before += len(text)
        after += len(out)
        src.write_text(out)
        print(f"{src.name}: {cf.group(1)}")
    print(f"sources: {before/1e6:.1f} MB -> {after/1e6:.1f} MB (text; binary is about 1/4.2)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
