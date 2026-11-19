# -*- coding: utf-8 -*-
"""
Builds firmware/laboo_face/gauge_assets.h for the car gauge (carbon / glowing sport style):
  * gauge_bg_rle   : the dial with the speed arc switched off
  * gauge_lit_rle  : only the glowing arc (green -> yellow -> orange), everything else black
  * glyphs for the live numbers
The firmware shows the lit arc only up to the current speed, draws the needle, hub and numbers.
Run:  python tools/make_gauge_assets.py   (needs Pillow + numpy). Font: Orbitron (SIL OFL).
"""
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = Path(__file__).parent
OUT = HERE.parent / "firmware" / "laboo_face" / "gauge_assets.h"
PREVIEW = HERE / "gauge_preview.png"
F700 = str(HERE / "fonts" / "orbitron-latin-700-normal.woff")
F900 = str(HERE / "fonts" / "orbitron-latin-900-normal.woff")

W = 360
CX, CY = 180, 168                 # dial centre (a bit high: the panels sit below)
S = 3                             # supersampling
VMAX = 220
A0, SWEEP = 135.0, 270.0          # bottom-left, clockwise over the top, to bottom-right
ARC_R0, ARC_R1 = 150, 161         # the glowing speed arc
GLOW_R0, GLOW_R1 = 138, 174       # area where the arc's glow may appear
NEEDLE_LEN = 134
SPEED_Y = 232                     # top of the big speed digits (inside the hex panel)
SMALL_Y = 302                     # temp / time row
TEMP_X, TIME_X = 146, 238
BAR_X0, BAR_X1, BAR_Y = 112, 170, 326

ORANGE = (255, 150, 30)


def ang(v):
    return A0 + SWEEP * v / VMAX


def pol(r, a, cx=CX, cy=CY):
    a = math.radians(a)
    return cx + r * math.cos(a), cy + r * math.sin(a)


def arc_colour(f):
    """green -> yellow -> orange along the arc"""
    stops = [(0.0, (30, 220, 50)), (0.35, (130, 230, 30)), (0.48, (240, 220, 20)), (0.62, (255, 150, 10)),
             (1.0, (255, 80, 0))]
    for (f0, c0), (f1, c1) in zip(stops, stops[1:]):
        if f <= f1:
            k = (f - f0) / (f1 - f0)
            return tuple(int(c0[i] + (c1[i] - c0[i]) * k) for i in range(3))
    return stops[-1][1]


def carbon(size):
    """dark carbon-fibre twill texture"""
    y, x = np.mgrid[0:size, 0:size]
    cell = 6 * S
    u, v = (x // cell), (y // cell)
    twill = ((u + v) % 2).astype(np.float32)
    fx = (x % cell) / cell
    fy = (y % cell) / cell
    shade = np.where(twill > 0, 0.6 + 0.4 * np.sin(fx * math.pi), 0.6 + 0.4 * np.sin(fy * math.pi))
    base = 17 + 17 * shade
    img = np.stack([base, base, base * 1.08], -1)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))


def text(d, t, x, y, size, col, font=F700, anchor="mm"):
    f = ImageFont.truetype(font, size * S)
    d.text((x * S, y * S), t, font=f, fill=col, anchor=anchor)


def background():
    big = W * S
    img = carbon(big)
    d = ImageDraw.Draw(img)
    # vignette towards the edge
    vig = Image.new("L", (big, big), 0)
    vd = ImageDraw.Draw(vig)
    for r in range(180, 0, -3):
        vd.ellipse([(180 - r) * S, (180 - r) * S, (180 + r) * S, (180 + r) * S], fill=int(255 * (1 - r / 180) ** 0.6))
    img = Image.composite(img, Image.new("RGB", img.size, (0, 0, 0)), vig.point(lambda p: 90 + p * 165 // 255))
    d = ImageDraw.Draw(img)
    # chrome bezel
    for i, r in enumerate(range(178, 168, -1)):
        k = 0.5 + 0.5 * math.cos(i / 10 * math.pi)
        c = int(70 + 120 * k)
        d.ellipse([(180 - r) * S, (180 - r) * S, (180 + r) * S, (180 + r) * S], outline=(c, c, c + 8), width=S)
    # dark inner dial disc
    r = 103
    d.ellipse([(CX - r) * S, (CY - r) * S, (CX + r) * S, (CY + r) * S], fill=(10, 10, 12), outline=(55, 55, 62), width=2 * S)
    # unlit arc track
    rr = (ARC_R0 + ARC_R1) / 2
    d.arc([(CX - rr) * S, (CY - rr) * S, (CX + rr) * S, (CY + rr) * S], start=A0, end=A0 + SWEEP,
          fill=(30, 34, 30), width=(ARC_R1 - ARC_R0) * S)
    # ticks
    for v in range(0, VMAX + 1, 5):
        major, mid = v % 20 == 0, v % 10 == 0
        L = 12 if major else (8 if mid else 5)
        wd = 3 if major else (2 if mid else 1)
        col = (235, 235, 240) if major else (150, 150, 160)
        x0, y0 = pol(146, ang(v))
        x1, y1 = pol(146 - L, ang(v))
        d.line([x0 * S, y0 * S, x1 * S, y1 * S], fill=col, width=wd * S)
    for v in range(0, VMAX + 1, 20):
        x, y = pol(116, ang(v))
        text(d, str(v), x, y, 16, (215, 215, 225))
    # hex speed panel
    hexp = [(116, 254), (138, 220), (222, 220), (244, 254), (222, 288), (138, 288)]
    glow = Image.new("RGB", img.size, (0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.polygon([(x * S, y * S) for x, y in hexp], outline=ORANGE, width=4 * S)
    glow = glow.filter(ImageFilter.GaussianBlur(5 * S))
    img = Image.fromarray(np.clip(np.asarray(img, np.int16) + np.asarray(glow, np.int16) // 2, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(img)
    d.polygon([(x * S, y * S) for x, y in hexp], fill=(8, 8, 10), outline=(230, 130, 25), width=2 * S)
    text(d, "km/h", 180, 277, 10, ORANGE)
    # temp: thermometer icon + bar track
    tx, ty = TEMP_X - 36, SMALL_Y + 8
    d.rounded_rectangle([(tx - 3) * S, (ty - 12) * S, (tx + 3) * S, (ty + 4) * S], radius=3 * S, outline=ORANGE, width=S * 2)
    d.ellipse([(tx - 6) * S, (ty + 1) * S, (tx + 6) * S, (ty + 13) * S], fill=ORANGE)
    for k in range(3):
        d.line([(tx + 5) * S, (ty - 9 + 4 * k) * S, (tx + 9) * S, (ty - 9 + 4 * k) * S], fill=ORANGE, width=S)
    text(d, "TEMP", TEMP_X, SMALL_Y - 9, 8, (190, 190, 200))
    d.rounded_rectangle([BAR_X0 * S, (BAR_Y - 2) * S, BAR_X1 * S, (BAR_Y + 2) * S], radius=2 * S, fill=(45, 45, 52))
    # time: clock icon
    cx2, cy2 = TIME_X - 44, SMALL_Y + 8
    d.ellipse([(cx2 - 9) * S, (cy2 - 9) * S, (cx2 + 9) * S, (cy2 + 9) * S], outline=(230, 230, 235), width=2 * S)
    d.line([cx2 * S, cy2 * S, cx2 * S, (cy2 - 6) * S], fill=(230, 230, 235), width=2 * S)
    d.line([cx2 * S, cy2 * S, (cx2 + 5) * S, cy2 * S], fill=(230, 230, 235), width=2 * S)
    text(d, "TIME", TIME_X, SMALL_Y - 9, 8, (190, 190, 200))
    # brand line
    d.line([150 * S, 342 * S, 162 * S, 342 * S], fill=ORANGE, width=2 * S)
    d.line([198 * S, 342 * S, 210 * S, 342 * S], fill=ORANGE, width=2 * S)
    text(d, "LABOO", 180, 342, 9, (200, 200, 210), F900)
    return img.resize((W, W), Image.LANCZOS)


def lit_arc():
    """the arc fully lit, with glow, on black"""
    big = W * S
    img = Image.new("RGB", (big, big), (0, 0, 0))
    d = ImageDraw.Draw(img)
    rr = (ARC_R0 + ARC_R1) / 2
    n = 270
    for i in range(n):
        a0 = A0 + SWEEP * i / n
        a1 = A0 + SWEEP * (i + 1.3) / n
        d.arc([(CX - rr) * S, (CY - rr) * S, (CX + rr) * S, (CY + rr) * S], start=a0, end=a1,
              fill=arc_colour(i / n), width=(ARC_R1 - ARC_R0) * S)
    glow = img.filter(ImageFilter.GaussianBlur(6 * S))
    core = np.asarray(img, np.float32)
    g = np.asarray(glow, np.float32)
    # bright centre line of the arc
    hl = Image.new("L", (big, big), 0)
    ImageDraw.Draw(hl).arc([(CX - rr) * S, (CY - rr) * S, (CX + rr) * S, (CY + rr) * S], start=A0, end=A0 + SWEEP,
                           fill=255, width=3 * S)
    hl = np.asarray(hl.filter(ImageFilter.GaussianBlur(S)), np.float32)[..., None] / 255
    out = np.clip(core * 0.9 + g * 1.1 + hl * 45, 0, 255).astype(np.uint8)
    res = Image.fromarray(out).resize((W, W), Image.LANCZOS)
    a = np.asarray(res).copy()
    a[a.max(axis=2) < 10] = 0                      # pure black outside the glow -> tiny RLE
    return Image.fromarray(a)


def rle(img):
    a = np.asarray(img.convert("RGB"), dtype=np.uint16)
    v = (((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)).ravel()
    starts = np.concatenate(([0], np.flatnonzero(np.diff(v)) + 1))
    lens = np.diff(np.concatenate((starts, [v.size])))
    out = bytearray()
    for s, n in zip(starts, lens):
        c = int(v[s])
        while n > 0:
            k = min(256, n)
            out += bytes((k - 1, c >> 8, c & 0xFF))
            n -= k
    return bytes(out)


def glyphs(font_path, size, chars):
    f = ImageFont.truetype(font_path, size)
    asc, desc = f.getmetrics()
    h = asc + desc
    res = {}
    for ch in chars:
        w = int(math.ceil(f.getlength(ch)))
        im = Image.new("L", (w, h), 0)
        ImageDraw.Draw(im).text((0, 0), ch, font=f, fill=255)
        res[ch] = (w, h, np.asarray(im))
    rows = np.zeros(h, bool)
    for w, hh, a in res.values():
        rows |= a.max(axis=1) > 0
    top, bot = int(np.argmax(rows)), int(h - np.argmax(rows[::-1]))
    return {ch: (w, bot - top, a[top:bot]) for ch, (w, hh, a) in res.items()}


def c_font(name, g, chars):
    data, offs = bytearray(), []
    for ch in chars:
        w, h, a = g[ch]
        offs.append((len(data), w))
        q = (a.astype(np.uint16) * 15 + 127) // 255
        flat = q.ravel()
        if flat.size % 2:
            flat = np.append(flat, 0)
        data += bytes(((flat[0::2] << 4) | flat[1::2]).astype(np.uint8))
    h = g[chars[0]][1]
    cchars = chars.replace("°", "d")          # '°' is written as 'd' in the firmware strings
    return "\n".join([
        f"static const uint8_t {name}_data[] = {{" + ",".join(str(b) for b in data) + "};",
        f"static const GaugeGlyph {name}_glyphs[] = {{" + ",".join(f"{{{o},{w}}}" for o, w in offs) + "};",
        f'static const GaugeFont {name} = {{"{cchars}", {h}, {name}_glyphs, {name}_data}};'])


def main():
    bg, lit = background(), lit_arc()
    bgd, litd = rle(bg), rle(lit)
    big = glyphs(F900, 40, "0123456789-")
    small = glyphs(F700, 19, "0123456789:-°")
    defs = dict(G_VMAX=VMAX, G_A0=f"{A0}f", G_SWEEP=f"{SWEEP}f", G_CX=CX, G_CY=CY, G_GLOW_R0=GLOW_R0,
                G_GLOW_R1=GLOW_R1, G_NEEDLE_LEN=NEEDLE_LEN, G_SPEED_Y=SPEED_Y, G_SMALL_Y=SMALL_Y,
                G_TEMP_X=TEMP_X, G_TIME_X=TIME_X, G_BAR_X0=BAR_X0, G_BAR_X1=BAR_X1, G_BAR_Y=BAR_Y)
    lines = ["// generated by tools/make_gauge_assets.py - do not edit", "#pragma once", "#include <stdint.h>"]
    lines += [f"#define {k} {v}" for k, v in defs.items()]
    lines += ["struct GaugeGlyph { uint32_t off; uint8_t w; };",
              "struct GaugeFont { const char* chars; uint8_t h; const GaugeGlyph* g; const uint8_t* data; };",
              f"static const uint32_t gauge_bg_len = {len(bgd)};",
              "static const uint8_t gauge_bg_rle[] = {" + ",".join(map(str, bgd)) + "};",
              f"static const uint32_t gauge_lit_len = {len(litd)};",
              "static const uint8_t gauge_lit_rle[] = {" + ",".join(map(str, litd)) + "};",
              c_font("gfont_big", big, "0123456789-"),
              c_font("gfont_small", small, "0123456789:-°")]
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    prev = Image.fromarray(np.where(np.asarray(lit).max(axis=2, keepdims=True) > 0, np.asarray(lit), np.asarray(bg)))
    prev.save(PREVIEW)
    print(f"bg {len(bgd)} bytes, lit {len(litd)} bytes -> {OUT}")


if __name__ == "__main__":
    main()
