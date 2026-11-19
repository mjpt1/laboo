# -*- coding: utf-8 -*-
"""
Pictures for the round 360x360 screen, drawn on the laptop with Pillow and sent to the board.
  clock   : time, Persian (Jalali) date, weekday, little weather
  weather : today + next days
  timer   : countdown with a progress ring
  message : a reminder / note card
  photo   : a picture from pc/photos/
"""
import math
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from knowledge import to_jalali, J_MONTHS, WEEKDAYS

HERE = Path(__file__).parent
FONT_DIR = HERE / "fonts"
W = H = 360
CX = CY = 180

FA_DIGITS = str.maketrans("0123456789", "۰۱۲۳۴۵۶۷۸۹")
EN_DAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]

# ------------------------------------------------------------------ text helpers
_fonts = {}


def font(size, bold=False):
    key = (size, bold)
    if key not in _fonts:
        names = (["Vazirmatn-Bold.ttf"] if bold else ["Vazirmatn-Regular.ttf"]) + ["Vazirmatn-Regular.ttf"]
        f = None
        for n in names:
            p = FONT_DIR / n
            if p.exists():
                f = ImageFont.truetype(str(p), size, layout_engine=ImageFont.Layout.BASIC)
                break
        if f is None:
            for p in (r"C:\Windows\Fonts\tahomabd.ttf" if bold else r"C:\Windows\Fonts\tahoma.ttf",
                      r"C:\Windows\Fonts\tahoma.ttf"):
                try:
                    f = ImageFont.truetype(p, size, layout_engine=ImageFont.Layout.BASIC)
                    break
                except Exception:
                    pass
        _fonts[key] = f or ImageFont.load_default()
    return _fonts[key]


def fa(text):
    """Persian text -> joined letters in visual order (Pillow draws left to right)."""
    try:
        import arabic_reshaper
        from bidi.algorithm import get_display
        return get_display(arabic_reshaper.reshape(text))
    except Exception:
        return text


def fa_num(x):
    return str(x).translate(FA_DIGITS)


def center_text(d, y, text, size, color, bold=False, persian=True):
    f = font(size, bold)
    t = fa(text) if persian else text
    w = d.textlength(t, font=f)
    d.text((CX - w / 2, y), t, font=f, fill=color)


def dim(color, k):
    return tuple(int(c * k) for c in color)


# ------------------------------------------------------------------ weather icons (drawn, no image files)
def icon(d, kind, x, y, s, night=False):
    """kind: clear, partly, cloudy, fog, drizzle, rain, snow, storm"""
    sun = (255, 196, 40)
    cloud = (200, 210, 225)
    if kind in ("clear", "partly"):
        cx, cy = (x - s * 0.15, y - s * 0.15) if kind == "partly" else (x, y)
        r = s * (0.28 if kind == "partly" else 0.34)
        if night:
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(235, 235, 210))
            d.ellipse([cx - r + r * 0.55, cy - r - r * 0.25, cx + r + r * 0.55, cy + r - r * 0.25], fill=(0, 0, 0))
        else:
            for k in range(8):
                a = k * math.pi / 4
                d.line([cx + math.cos(a) * r * 1.25, cy + math.sin(a) * r * 1.25,
                        cx + math.cos(a) * r * 1.65, cy + math.sin(a) * r * 1.65], fill=sun, width=max(2, int(s / 22)))
            d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=sun)
        if kind == "clear":
            return
    if kind != "clear":
        # a cloud made of three circles
        bx, by = x + (s * 0.08 if kind == "partly" else 0), y + (s * 0.08 if kind == "partly" else 0)
        c = cloud if kind not in ("rain", "storm") else (150, 160, 180)
        d.ellipse([bx - s * 0.42, by - s * 0.05, bx - s * 0.06, by + s * 0.28], fill=c)
        d.ellipse([bx - s * 0.22, by - s * 0.28, bx + s * 0.18, by + s * 0.12], fill=c)
        d.ellipse([bx + s * 0.02, by - s * 0.12, bx + s * 0.42, by + s * 0.28], fill=c)
        d.rectangle([bx - s * 0.24, by + s * 0.06, bx + s * 0.24, by + s * 0.28], fill=c)
        if kind in ("drizzle", "rain"):
            for k in range(3 if kind == "rain" else 2):
                xx = bx - s * 0.2 + k * s * 0.2
                d.line([xx, by + s * 0.36, xx - s * 0.06, by + s * 0.5], fill=(90, 160, 255), width=max(2, int(s / 18)))
        elif kind == "snow":
            for k in range(3):
                xx = bx - s * 0.2 + k * s * 0.2
                d.ellipse([xx - s * 0.04, by + s * 0.38, xx + s * 0.04, by + s * 0.46], fill=(255, 255, 255))
        elif kind == "storm":
            d.polygon([(bx, by + s * 0.3), (bx - s * 0.1, by + s * 0.5), (bx + s * 0.02, by + s * 0.48),
                       (bx - s * 0.06, by + s * 0.66), (bx + s * 0.14, by + s * 0.42), (bx + s * 0.02, by + s * 0.44)],
                      fill=(255, 220, 60))
        elif kind == "fog":
            for k in range(3):
                yy = by + s * 0.38 + k * s * 0.1
                d.line([bx - s * 0.36, yy, bx + s * 0.36, yy], fill=(170, 170, 170), width=max(2, int(s / 20)))


# ------------------------------------------------------------------ screens
def _base():
    img = Image.new("RGB", (W, H), (0, 0, 0))
    return img, ImageDraw.Draw(img)


def clock(now=None, weather=None, night=False, persian_digits=True, note=None):
    now = now or datetime.now()
    img, d = _base()
    k = 0.55 if night else 1.0
    white, gray, accent = dim((255, 255, 255), k), dim((150, 160, 175), k), dim((255, 190, 60), k)
    hm = now.strftime("%H:%M")
    center_text(d, 92, fa_num(hm) if persian_digits else hm, 104, white, bold=True, persian=False)
    jy, jm, jd = to_jalali(now.year, now.month, now.day)
    center_text(d, 216, WEEKDAYS[now.weekday()], 30, accent, bold=True)
    center_text(d, 254, f"{fa_num(jd)} {J_MONTHS[jm - 1]} {fa_num(jy)}", 26, white)
    center_text(d, 294, now.strftime("%d %b %Y"), 16, gray, persian=False)
    if weather and weather.get("temp") is not None:
        t = f"{fa_num(round(weather['temp']))}°"
        f = font(28, True)
        tw = d.textlength(t, font=f)
        icon(d, weather.get("kind", "clear"), CX - 30, 64, 44, night=weather.get("night", False))
        d.text((CX + 4, 44), t, font=f, fill=white)
    if note:
        center_text(d, 318, note, 16, accent)
    return img


def weather_screen(weather, night=False):
    img, d = _base()
    k = 0.6 if night else 1.0
    white, gray, accent = dim((255, 255, 255), k), dim((150, 160, 175), k), dim((120, 190, 255), k)
    if not weather or weather.get("temp") is None:
        center_text(d, 150, "اطلاعات آب‌وهوا در دسترس نیست", 22, gray)
        return img
    icon(d, weather.get("kind", "clear"), CX, 76, 84, night=weather.get("night", False))
    center_text(d, 118, f"{fa_num(round(weather['temp']))}°", 64, white, bold=True, persian=False)
    center_text(d, 198, weather.get("desc", ""), 24, accent, bold=True)
    if weather.get("tmax") is not None:
        center_text(d, 232, f"بیشینه {fa_num(round(weather['tmax']))}  ،  کمینه {fa_num(round(weather['tmin']))}", 17, gray)
    days = weather.get("days") or []
    if days:
        n = min(3, len(days))
        f = font(14)
        for i, day in enumerate(days[:n]):
            x = CX - (i - (n - 1) / 2) * 76           # right to left, like Persian
            icon(d, day["kind"], x, 272, 26)
            tt = f"{fa_num(round(day['tmax']))}°"
            d.text((x - d.textlength(tt, font=f) / 2, 288), tt, font=f, fill=white)
            nm = fa(day["name"])
            d.text((x - d.textlength(nm, font=f) / 2, 308), nm, font=f, fill=gray)
    return img


def timer_screen(remaining, total, label="", night=False):
    img, d = _base()
    k = 0.6 if night else 1.0
    white, gray, ring, track = dim((255, 255, 255), k), dim((150, 160, 175), k), dim((255, 140, 40), k), dim((45, 45, 55), k)
    m, s = divmod(max(0, int(round(remaining))), 60)
    h, m = divmod(m, 60)
    txt = f"{h}:{m:02d}:{s:02d}" if h else f"{m:02d}:{s:02d}"
    r = 160
    d.ellipse([CX - r, CY - r, CX + r, CY + r], outline=track, width=14)
    frac = 0 if total <= 0 else max(0.0, min(1.0, remaining / total))
    if frac > 0:
        d.arc([CX - r, CY - r, CX + r, CY + r], start=-90, end=-90 + 360 * frac, fill=ring, width=14)
    center_text(d, 120, fa_num(txt), 84 if not h else 62, white, bold=True, persian=False)
    center_text(d, 232, label or "تایمر", 24, gray)
    return img


def message_screen(title, text, night=False):
    img, d = _base()
    k = 0.6 if night else 1.0
    white, accent = dim((255, 255, 255), k), dim((255, 190, 60), k)
    # bell
    d.ellipse([CX - 22, 52, CX + 22, 96], fill=accent)
    d.rectangle([CX - 22, 74, CX + 22, 100], fill=accent)
    d.rectangle([CX - 30, 98, CX + 30, 106], fill=accent)
    d.ellipse([CX - 6, 104, CX + 6, 116], fill=accent)
    center_text(d, 124, title, 26, accent, bold=True)
    # wrap the text into lines of ~22 characters
    words, lines, cur = text.split(), [], ""
    for w_ in words:
        if len(cur) + len(w_) + 1 > 22 and cur:
            lines.append(cur)
            cur = w_
        else:
            cur = (cur + " " + w_).strip()
    if cur:
        lines.append(cur)
    y = 172
    for ln in lines[:4]:
        center_text(d, y, ln, 24, white)
        y += 36
    return img


def photo_screen(path):
    im = Image.open(path).convert("RGB")
    s = min(im.size)
    left, top = (im.width - s) // 2, (im.height - s) // 2
    return im.crop((left, top, left + s, top + s)).resize((W, H), Image.LANCZOS)


# ------------------------------------------------------------------ to the board's format
def encode(img):
    """RGB image -> RLE bytes: [count-1][hi][lo] per run of equal RGB565 pixels (what the firmware expects)."""
    a = np.asarray(img.convert("RGB"), dtype=np.uint16)
    v = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 2) << 5) | (a[..., 2] >> 3)
    v = v.ravel()
    starts = np.concatenate(([0], np.flatnonzero(np.diff(v)) + 1))
    lens = np.diff(np.concatenate((starts, [v.size])))
    vals = v[starts]
    reps = (lens + 255) // 256                        # runs longer than 256 are split
    vals_r = np.repeat(vals, reps)
    cnt = np.full(reps.sum(), 256, dtype=np.int64)
    ends = np.cumsum(reps) - 1
    cnt[ends] = lens - (reps - 1) * 256
    out = np.empty((cnt.size, 3), dtype=np.uint8)
    out[:, 0] = cnt - 1
    out[:, 1] = vals_r >> 8
    out[:, 2] = vals_r & 0xFF
    return out.tobytes()


def decode(data):
    """For testing: RLE bytes -> RGB image."""
    b = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3)
    v = np.repeat((b[:, 1].astype(np.uint16) << 8) | b[:, 2], b[:, 0].astype(np.int64) + 1)[:W * H]
    r = ((v >> 11) & 31) << 3
    g = ((v >> 5) & 63) << 2
    bl = (v & 31) << 3
    return Image.fromarray(np.stack([r, g, bl], -1).reshape(H, W, 3).astype(np.uint8))
