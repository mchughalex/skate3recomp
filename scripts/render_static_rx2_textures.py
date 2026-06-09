#!/usr/bin/env python3
"""Extract Skate 3 EB archives and render top-mip RX2 textures to PNG."""

from __future__ import annotations

import argparse
import html
import math
import shutil
import struct
import subprocess
from pathlib import Path

from PIL import Image


TEXTURE_FORMATS = {
    0x52: ("DXT1", 8),
    0x53: ("DXT3", 16),
    0x54: ("DXT5", 16),
}


def safe_output_stem(path: Path, root: Path | None = None) -> str:
    try:
        label = path.relative_to(root) if root else path
    except ValueError:
        label = path
    return "__".join(part.replace("/", "_").replace("\\", "_") for part in label.with_suffix("").parts)


def tiled_offset_2d_row(y: int, width: int, log2_bpp: int) -> int:
    macro = ((y // 32) * (width // 32)) << (log2_bpp + 7)
    micro = ((y & 6) << 2) << log2_bpp
    return (
        macro
        + ((micro & ~0xF) << 1)
        + (micro & 0xF)
        + ((y & 8) << (3 + log2_bpp))
        + ((y & 1) << 4)
    )


def tiled_offset_2d_column(x: int, y: int, log2_bpp: int, base_offset: int) -> int:
    macro = (x // 32) << (log2_bpp + 7)
    micro = (x & 7) << log2_bpp
    offset = base_offset + (macro + ((micro & ~0xF) << 1) + (micro & 0xF))
    return (
        ((offset & ~0x1FF) << 3)
        + ((offset & 0x1C0) << 2)
        + (offset & 0x3F)
        + ((y & 16) << 7)
        + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)
    )


def swap16(data: bytes) -> bytes:
    output = bytearray(data)
    for offset in range(0, len(output) - 1, 2):
        output[offset], output[offset + 1] = output[offset + 1], output[offset]
    return bytes(output)


def untile_blocks(data: bytes, blocks_w: int, blocks_h: int, bytes_per_block: int) -> bytes:
    log2_bpp = 3 if bytes_per_block == 8 else 4
    pitch_blocks = max(32, math.ceil(blocks_w / 32) * 32)
    output = bytearray(blocks_w * blocks_h * bytes_per_block)
    for y in range(blocks_h):
        row_offset = tiled_offset_2d_row(y, pitch_blocks, log2_bpp)
        for x in range(blocks_w):
            input_offset = tiled_offset_2d_column(x, y, log2_bpp, row_offset)
            input_offset = (input_offset >> log2_bpp) * bytes_per_block
            output_offset = (y * blocks_w + x) * bytes_per_block
            if input_offset + bytes_per_block <= len(data):
                output[output_offset : output_offset + bytes_per_block] = data[
                    input_offset : input_offset + bytes_per_block
                ]
    return swap16(output)


def make_dds(width: int, height: int, fourcc: str, pitch_or_size: int, payload: bytes) -> bytes:
    header = bytearray(b"DDS ")
    header += struct.pack("<I", 124)
    header += struct.pack("<I", 0x0002100F)
    header += struct.pack("<I", height)
    header += struct.pack("<I", width)
    header += struct.pack("<I", pitch_or_size)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 1)
    header += bytes(44)
    header += struct.pack("<I", 32)
    header += struct.pack("<I", 0x4)
    header += fourcc.encode("ascii")
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0x1000)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    header += struct.pack("<I", 0)
    return bytes(header) + payload


def parse_rx2_header(data: bytes) -> tuple[int, int, int, int] | None:
    if not data.startswith(b"\x89RW4xb2") or len(data) < 0x238:
        return None
    data_offset = int.from_bytes(data[0x44:0x48], "big")
    if data_offset <= 0 or data_offset >= len(data):
        data_offset = 0x238
    fmt = data[0x167]
    height = (int.from_bytes(data[0x168:0x16A], "big") + 1) * 8
    width = (int.from_bytes(data[0x16A:0x16C], "big") + 1) & 0x1FFF
    if width <= 0 or height <= 0 or width > 8192 or height > 8192:
        return None
    return fmt, width, height, data_offset


def render_rx2(
    path: Path,
    output_dir: Path,
    root: Path | None = None,
    layout: str = "tiled",
) -> Path | None:
    output_path = output_dir / "images" / (safe_output_stem(path, root) + ".png")
    if output_path.exists():
        return output_path

    data = path.read_bytes()
    parsed = parse_rx2_header(data)
    if not parsed:
        return None
    fmt, width, height, data_offset = parsed
    format_info = TEXTURE_FORMATS.get(fmt)
    if not format_info:
        return None

    fourcc, bytes_per_block = format_info
    blocks_w = max(1, math.ceil(width / 4))
    blocks_h = max(1, math.ceil(height / 4))
    top_mip_size = blocks_w * blocks_h * bytes_per_block
    payload = data[data_offset : data_offset + top_mip_size]
    if len(payload) < top_mip_size:
        return None

    if layout == "linear":
        decoded = swap16(payload)
    elif layout == "tiled":
        decoded = untile_blocks(payload, blocks_w, blocks_h, bytes_per_block)
    else:
        raise ValueError(f"Unknown RX2 layout: {layout}")

    dds = make_dds(width, height, fourcc, top_mip_size, decoded)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    Image.open(__import__("io").BytesIO(dds)).convert("RGBA").save(output_path)
    return output_path


def display_source_name(source: Path, root: Path | None) -> str:
    try:
        return str(source.relative_to(root)) if root else str(source)
    except ValueError:
        return str(source)


def write_texture_page(
    rendered: list[tuple[Path, Path, str]],
    output_dir: Path,
    page_number: int,
    page_count: int,
) -> str:
    page_name = f"page-{page_number:03d}.html"
    rows = [
        "<!doctype html><meta charset='utf-8'><title>Skate 3 RX2 textures</title>",
        "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:24px}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:14px}"
        ".item{background:#1b1b1b;padding:8px;border-radius:4px;overflow:hidden}"
        "img{width:100%;height:auto;max-height:240px;object-fit:contain;background:#333;image-rendering:auto}"
        "code{font-size:11px;word-break:break-all;color:#cceeff}"
        "a{color:#9bdcff}.nav{display:flex;gap:12px;align-items:center;margin:12px 0 20px}</style>",
        f"<h1>Skate 3 RX2 Textures: Page {page_number} / {page_count}</h1>",
        "<div class='nav'>",
        "<a href='index.html'>Index</a>",
    ]
    if page_number > 1:
        rows.append(f"<a href='page-{page_number - 1:03d}.html'>Previous</a>")
    if page_number < page_count:
        rows.append(f"<a href='page-{page_number + 1:03d}.html'>Next</a>")
    rows.append("</div><div class='grid'>")
    for source, image, label in rendered:
        rel_image = image.relative_to(output_dir)
        rows.append(
            f"<div class='item'><img loading='lazy' src='{html.escape(str(rel_image))}'>"
            f"<p><code>{html.escape(label)}</code></p></div>"
        )
    rows.append("</div>")
    (output_dir / page_name).write_text("\n".join(rows), encoding="utf-8")
    return page_name


def write_index(rendered: list[tuple[Path, Path, str]], output_dir: Path, page_size: int) -> None:
    page_size = max(1, page_size)
    pages = [rendered[offset : offset + page_size] for offset in range(0, len(rendered), page_size)]
    page_names = [
        write_texture_page(page, output_dir, page_number, len(pages))
        for page_number, page in enumerate(pages, start=1)
    ]

    rows = [
        "<!doctype html><meta charset='utf-8'><title>Skate 3 RX2 textures</title>",
        "<style>body{font-family:sans-serif;background:#111;color:#eee;margin:24px}"
        "a{color:#9bdcff}.pages{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:10px}"
        ".page{background:#1b1b1b;padding:12px;border-radius:4px}</style>",
        "<h1>Skate 3 RX2 Textures</h1>",
        f"<p>{len(rendered)} textures rendered. Open one page at a time to avoid loading every PNG at once.</p>",
        "<div class='pages'>",
    ]
    for index, page_name in enumerate(page_names, start=1):
        start = ((index - 1) * page_size) + 1
        end = min(index * page_size, len(rendered))
        rows.append(
            f"<a class='page' href='{page_name}'>Page {index}<br>{start}-{end}</a>"
        )
    rows.append("</div>")
    (output_dir / "index.html").write_text("\n".join(rows), encoding="utf-8")


def maybe_extract_big(args: argparse.Namespace) -> Path:
    extracted_dir = Path(args.extracted_dir)
    texture_dir = extracted_dir / "data/content/createacharacter/texture"
    if texture_dir.exists() and list(texture_dir.glob("*.rx2")):
        return extracted_dir

    if not args.big:
        raise SystemExit("No extracted textures found and --big was not provided.")
    quickbms = shutil.which(args.quickbms)
    if not quickbms:
        raise SystemExit(f"QuickBMS executable not found: {args.quickbms}")
    script = Path(args.bms_script)
    if not script.exists():
        raise SystemExit(f"BMS script not found: {script}")

    extracted_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run([quickbms, "-Y", str(script), str(args.big), str(extracted_dir)], check=True)
    return extracted_dir


def read_paths_file(path: Path) -> list[Path]:
    files: list[Path] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        files.append(Path(line))
    return files


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--big", type=Path, help="Optional createacharacter.big path to extract first")
    parser.add_argument("--quickbms", default="/tmp/quickbms_skate/quickbms_linux/quickbms")
    parser.add_argument("--bms-script", default="/tmp/quickbms_skate/fightnight.bms")
    parser.add_argument("--extracted-dir", default="/tmp/skate_static_rx2")
    parser.add_argument(
        "--texture-dir",
        type=Path,
        help="Directory containing RX2 files to render. Defaults to createacharacter/texture.",
    )
    parser.add_argument(
        "--paths-file",
        type=Path,
        help="Text file containing explicit RX2 paths to render, one per line.",
    )
    parser.add_argument("--recursive", action="store_true", help="Recursively scan --texture-dir")
    parser.add_argument(
        "--layout",
        choices=("tiled", "linear"),
        default="tiled",
        help="RX2 memory layout. createacharacter textures are tiled; FE/menu textures are usually linear.",
    )
    parser.add_argument("--output-dir", default="/tmp/skate_static_rx2_rendered")
    parser.add_argument("--limit", type=int, default=500, help="Maximum textures to render")
    parser.add_argument("--page-size", type=int, default=100, help="Textures per HTML page")
    parser.add_argument("--verbose", action="store_true", help="Print every rendered texture")
    args = parser.parse_args()

    extracted_dir = Path(args.extracted_dir)
    if args.paths_file:
        sources = read_paths_file(args.paths_file)
        source_root = None
    else:
        if args.texture_dir:
            texture_dir = args.texture_dir
            source_root = texture_dir
        else:
            extracted_dir = maybe_extract_big(args)
            texture_dir = extracted_dir / "data/content/createacharacter/texture"
            source_root = texture_dir
        globber = texture_dir.rglob if args.recursive else texture_dir.glob
        sources = sorted(globber("*.rx2"))

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    rendered: list[tuple[Path, Path, str]] = []
    for source in sources:
        image = render_rx2(source, output_dir, source_root, args.layout)
        if image:
            rendered.append((source, image, display_source_name(source, source_root)))
            if args.verbose:
                print(f"Rendered {source.name}")
        if args.limit and len(rendered) >= args.limit:
            break

    write_index(rendered, output_dir, args.page_size)
    print(f"Wrote {len(rendered)} textures to {output_dir}")
    print(f"Open {output_dir / 'index.html'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
