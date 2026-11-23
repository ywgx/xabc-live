#!/usr/bin/env python3
"""Regenerate assets/fonts/live_cjk_16.c (ChatGPT UI font) with lv_font_conv.

The UI shows only a short status line, so the font holds exactly the characters
used by string literals in main/app_*.c and main/main.c, plus printable ASCII and
common Chinese punctuation. Rerun after changing any on-screen Chinese text.
Usage: python3 tools/gen_live_font.py --font NotoSansSC-Regular.otf --conv <lv_font_conv>
"""
import argparse, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def charset() -> str:
    chars = set(chr(c) for c in range(0x21, 0x7F))
    chars.update("，。：；、？！“”‘’（）…—·%")
    sources = sorted((ROOT / "main").glob("app_*.c")) + [ROOT / "main" / "main.c"]
    for path in sources:
        for literal in STRING.findall(path.read_text(encoding="utf-8")):
            chars.update(ch for ch in literal if ord(ch) > 0x7F)
    return "".join(sorted(chars))

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True)
    ap.add_argument("--conv", default="lv_font_conv")
    args = ap.parse_args()
    symbols = charset()
    out = ROOT / "assets/fonts/live_cjk_16.c"
    cmd = [args.conv, "--font", args.font, "-r", "0x20", "--symbols", symbols,
           "--size", "16", "--bpp", "4", "--format", "lvgl", "--no-compress",
           "--lv-include", "lvgl.h", "-o", str(out)]
    subprocess.run(cmd, check=True)
    # Drop the symbol list from the header comment: it is reproducible from this script.
    text = out.read_text(encoding="utf-8")
    text = text.replace(symbols, "<see tools/gen_live_font.py>")
    text = text.replace(str(out), "assets/fonts/live_cjk_16.c")
    text = text.replace(args.font, Path(args.font).name)
    out.write_text(text, encoding="utf-8")
    print(f"{out.relative_to(ROOT)}: {len(symbols)} symbols + space")
    return 0

if __name__ == "__main__":
    sys.exit(main())
