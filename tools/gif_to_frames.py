"""
Convert GIF(s) to RGB565 frame arrays for the Davey Jones easter egg player.

Usage:
    python gif_to_frames.py <gif1> [gif2] [--landscape] [--max-frames 30]

Output: components/nesso_ui/src/easter_egg_frames.h

Each GIF is resized to fit the LCD (135x240 portrait or 240x135 landscape),
converted to RGB565 little-endian, and embedded as a C array.
"""

import sys
import struct
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("pip install Pillow")
    sys.exit(1)

def gif_to_frames(gif_path, width, height, max_frames=30):
    """Extract frames from a GIF, resize, convert to RGB565 LE."""
    img = Image.open(gif_path)
    frames = []
    try:
        while len(frames) < max_frames:
            frame = img.convert("RGB").resize((width, height), Image.LANCZOS)
            pixels = frame.load()
            data = bytearray()
            for y in range(height):
                for x in range(width):
                    r, g, b = pixels[x, y]
                    rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                    data += struct.pack("<H", rgb565)
            frames.append(bytes(data))
            img.seek(img.tell() + 1)
    except EOFError:
        pass
    return frames

def write_header(all_frames, gif_names, width, height, out_path):
    """Write C header with frame arrays."""
    with open(out_path, "w") as f:
        f.write("/* Auto-generated easter egg frames — DO NOT EDIT */\n")
        f.write(f"#define EGG_W {width}\n")
        f.write(f"#define EGG_H {height}\n")
        f.write(f"#define EGG_FRAME_BYTES ({width * height * 2})\n\n")

        total = 0
        for gi, (name, frames) in enumerate(zip(gif_names, all_frames)):
            for fi, frame in enumerate(frames):
                arr_name = f"egg_gif{gi}_f{fi}"
                f.write(f"static const uint8_t {arr_name}[{len(frame)}] = {{\n")
                for i in range(0, len(frame), 16):
                    chunk = frame[i:i+16]
                    f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
                f.write("};\n\n")
                total += 1

        # Frame pointer table per gif
        for gi, frames in enumerate(all_frames):
            f.write(f"static const uint8_t *egg_gif{gi}[] = {{\n")
            for fi in range(len(frames)):
                f.write(f"    egg_gif{gi}_f{fi},\n")
            f.write("};\n")
            f.write(f"#define EGG_GIF{gi}_COUNT {len(frames)}\n\n")

        f.write(f"#define EGG_GIF_TOTAL {len(all_frames)}\n")
        f.write(f"/* Total frames across all GIFs: {total} */\n")

    print(f"Written: {out_path}")
    print(f"  {len(all_frames)} GIF(s), {total} total frames, {width}x{height}")

if __name__ == "__main__":
    landscape = "--landscape" in sys.argv
    max_frames = 30
    for i, a in enumerate(sys.argv):
        if a == "--max-frames" and i + 1 < len(sys.argv):
            max_frames = int(sys.argv[i + 1])

    gifs = [a for a in sys.argv[1:] if not a.startswith("--") and Path(a).exists()]
    if not gifs:
        print("Usage: python gif_to_frames.py <gif1> [gif2] [--landscape] [--max-frames 30]")
        sys.exit(1)

    w, h = (240, 135) if landscape else (135, 240)

    all_frames = []
    names = []
    for g in gifs:
        print(f"Processing: {g}")
        frames = gif_to_frames(g, w, h, max_frames)
        print(f"  {len(frames)} frames extracted")
        all_frames.append(frames)
        names.append(Path(g).stem)

    out = str(Path(__file__).parent.parent / "components" / "nesso_ui" / "src" / "easter_egg_frames.h")
    write_header(all_frames, names, w, h, out)
    print("\nDone! Rebuild and flash to see it.")
