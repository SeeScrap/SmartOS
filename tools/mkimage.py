"""Turns a picture into a C source file the firmware can draw (stage 6.2).

    python tools/mkimage.py wallpaper.jpg src/gfx/wallpaper_data.cpp
    python tools/mkimage.py wallpaper.png src/gfx/wallpaper_data.cpp --dim 45

The board has no JPEG or PNG decoder: decoding one would cost code, RAM and
time on every boot, for a picture that never changes. So the picture is
prepared here, on a computer, and arrives in the firmware as what the panel
actually wants - RGB565, already byte-swapped into the wire order src/gfx/fb.h
stores (see the note there), ready to be copied a row at a time.

    --w, --h    the size to produce (default 320x216, Home's area)
    --dim PCT   scale every pixel's brightness to this (default 55). Text is
                drawn over this picture; a photograph at full brightness eats
                white text alive, and dimming here costs the board nothing at
                run time.
    --name      the C symbol (default g_wallpaper)

The picture is fitted the way a phone fits a wallpaper: scaled until it covers
the area, then centred and cropped - never squashed.

Needs Pillow (`pip install pillow`) for JPEG and for good resampling. Without
it, 8-bit PNG still works through a decoder built in below, with nearest
-neighbour scaling.
"""
import struct
import sys
import zlib

DEF_W, DEF_H, DEF_DIM = 320, 216, 55


# --- reading a picture -------------------------------------------------------

def read_with_pillow(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    return im.width, im.height, im.tobytes()


def read_png(path):
    """Enough PNG for the pictures this tool is given: 8 bits a channel,
    colour types 0 (grey), 2 (RGB), 3 (palette) and 6 (RGBA), no interlace."""
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG, and Pillow is not installed")
    w = h = depth = ctype = None
    idat, plte = b"", b""
    i = 8
    while i < len(d):
        ln = struct.unpack(">I", d[i:i + 4])[0]
        kind, data = d[i + 4:i + 8], d[i + 8:i + 8 + ln]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", data[:13])
            if depth != 8 or interlace:
                raise SystemExit("only 8-bit, non-interlaced PNG without Pillow")
        elif kind == b"PLTE":
            plte = data
        elif kind == b"IDAT":
            idat += data
        i += 12 + ln

    chans = {0: 1, 2: 3, 3: 1, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * chans
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        for x in range(stride):
            a = line[x - chans] if x >= chans else 0
            b = prev[x]
            c = prev[x - chans] if x >= chans else 0
            if f == 1:   line[x] = (line[x] + a) & 0xFF
            elif f == 2: line[x] = (line[x] + b) & 0xFF
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        prev = line
        for x in range(w):
            if ctype == 2:   out += line[x * 3:x * 3 + 3]
            elif ctype == 6: out += line[x * 4:x * 4 + 3]
            elif ctype == 0: out += bytes((line[x],)) * 3
            else:            out += plte[line[x] * 3:line[x] * 3 + 3]
    return w, h, bytes(out)


def load(path):
    try:
        return read_with_pillow(path)
    except ImportError:
        return read_png(path)


# --- fitting and converting ---------------------------------------------------

def cover(src_w, src_h, src, w, h):
    """Scaled until it covers w x h, centred, cropped. Pillow does it properly
    when it is there; nearest neighbour when it is not."""
    try:
        from PIL import Image
        im = Image.frombytes("RGB", (src_w, src_h), src)
        scale = max(w / src_w, h / src_h)
        nw, nh = max(w, int(src_w * scale + 0.5)), max(h, int(src_h * scale + 0.5))
        im = im.resize((nw, nh), Image.LANCZOS)
        left, top = (nw - w) // 2, (nh - h) // 2
        return im.crop((left, top, left + w, top + h)).tobytes()
    except ImportError:
        pass
    scale = max(w / src_w, h / src_h)
    nw, nh = int(src_w * scale + 0.5), int(src_h * scale + 0.5)
    ox, oy = (nw - w) // 2, (nh - h) // 2
    out = bytearray()
    for y in range(h):
        sy = min(src_h - 1, int((y + oy) / scale))
        for x in range(w):
            sx = min(src_w - 1, int((x + ox) / scale))
            i = (sy * src_w + sx) * 3
            out += src[i:i + 3]
    return bytes(out)


def to_rgb565_wire(rgb, dim):
    """RGB565, byte-swapped: exactly what fb_rgb() in src/gfx/fb.h produces, so
    the firmware can copy these straight into the framebuffer."""
    out = []
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
        r, g, b = r * dim // 100, g * dim // 100, b * dim // 100
        c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out.append(((c >> 8) | (c << 8)) & 0xFFFF)
    return out


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 1
    src_path, out_path = args[0], args[1]
    w, h, dim, name = DEF_W, DEF_H, DEF_DIM, "g_wallpaper"
    i = 2
    while i < len(args):
        if args[i] == "--w":      w = int(args[i + 1]); i += 2
        elif args[i] == "--h":    h = int(args[i + 1]); i += 2
        elif args[i] == "--dim":  dim = int(args[i + 1]); i += 2
        elif args[i] == "--name": name = args[i + 1]; i += 2
        else: raise SystemExit(f"unknown option {args[i]}")

    sw, sh, rgb = load(src_path)
    px = to_rgb565_wire(cover(sw, sh, rgb, w, h), dim)

    body = []
    for row in range(h):
        line = ", ".join(f"0x{v:04x}" for v in px[row * w:(row + 1) * w])
        body.append(f"    {line},")
    with open(out_path, "w", newline="") as f:
        f.write(f'''// Generated by tools/mkimage.py - do not edit.
//
//     {" ".join(sys.argv[1:])}
//
// {src_path}: {sw}x{sh} -> {w}x{h}, covered and cropped, brightness {dim}%.
// RGB565 in the panel's wire order (see src/gfx/fb.h), {w * h * 2} bytes, in
// flash: it is read straight out of there a row at a time, never copied to RAM.

#include "gfx/image.h"

static const uint16_t s_px[{w} * {h}] = {{
{chr(10).join(body)}
}};

// extern, because a const at namespace scope is internal to its file in C++
// unless it is said out loud - and this one is reached from another file.
extern const image_t {name};
const image_t {name} = {{ {w}, {h}, s_px }};
''')
    print(f"{out_path}: {w}x{h}, {w * h * 2} bytes, from {src_path} ({sw}x{sh}), dim {dim}%")
    return 0


sys.exit(main())
