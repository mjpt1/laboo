# -*- coding: utf-8 -*-
"""
Extra abilities of the robot:
  * idle screen  : clock + Persian date + weather on the round screen when nobody talks to it
  * weather      : open-meteo.com (free, no key), refreshed every 20 minutes
  * timers       : "۵ دقیقه تایمر بذار", pomodoro, alarm sound + voice at the end
  * reminders    : "ساعت ۵ یادم بنداز ..." (kept in reminders.json, survive a restart)
  * photo frame  : pictures from pc/photos/ (swipe on the screen to change page)
  * laptop       : open websites / apps, media keys (play, next, volume), lock the screen
  * well-being   : morning greeting, "take a break" after a long time at the desk, special days
"""
import json
import math
import os
import re
import threading
import time
import uuid
import webbrowser
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent


def _log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


# ================================================================== weather
WMO = {  # code -> (Persian description, icon kind)
    0: ("آفتابی", "clear"), 1: ("بیشتر آفتابی", "partly"), 2: ("نیمه‌ابری", "partly"), 3: ("ابری", "cloudy"),
    45: ("مه‌آلود", "fog"), 48: ("مه‌آلود", "fog"),
    51: ("نم‌نم باران", "drizzle"), 53: ("نم‌نم باران", "drizzle"), 55: ("نم‌نم باران", "drizzle"),
    56: ("باران یخ‌زده", "drizzle"), 57: ("باران یخ‌زده", "drizzle"),
    61: ("باران سبک", "rain"), 63: ("بارانی", "rain"), 65: ("باران شدید", "rain"),
    66: ("باران یخ‌زده", "rain"), 67: ("باران یخ‌زده", "rain"),
    71: ("برف سبک", "snow"), 73: ("برفی", "snow"), 75: ("برف شدید", "snow"), 77: ("برف ریز", "snow"),
    80: ("رگبار", "rain"), 81: ("رگبار", "rain"), 82: ("رگبار شدید", "rain"),
    85: ("رگبار برف", "snow"), 86: ("رگبار برف", "snow"),
    95: ("رعد و برق", "storm"), 96: ("رعد و برق و تگرگ", "storm"), 99: ("رعد و برق و تگرگ", "storm"),
}
FA_WEEKDAYS = ["دوشنبه", "سه‌شنبه", "چهارشنبه", "پنجشنبه", "جمعه", "شنبه", "یکشنبه"]


class Weather:
    def __init__(self, cfg):
        self.cfg = cfg or {}
        self.lat = float(self.cfg.get("lat", 35.6892))
        self.lon = float(self.cfg.get("lon", 51.3890))
        self.city = self.cfg.get("city", "تهران")
        self.data = None
        self.last = 0
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            try:
                self.refresh()
            except Exception as e:
                _log(f"weather: could not update ({str(e)[:80]})")
            time.sleep(20 * 60 if self.data else 120)

    def refresh(self):
        import urllib.request
        url = ("https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
               "&current=temperature_2m,relative_humidity_2m,weather_code,is_day,wind_speed_10m"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=4"
               % (self.lat, self.lon))
        with urllib.request.urlopen(url, timeout=15) as r:
            j = json.loads(r.read().decode("utf-8"))
        cur, daily = j["current"], j["daily"]
        desc, kind = WMO.get(int(cur["weather_code"]), ("", "cloudy"))
        days = []
        for i in range(1, len(daily["time"])):
            dt = datetime.strptime(daily["time"][i], "%Y-%m-%d")
            dd, dk = WMO.get(int(daily["weather_code"][i]), ("", "cloudy"))
            days.append({"name": FA_WEEKDAYS[dt.weekday()], "kind": dk, "desc": dd,
                         "tmax": daily["temperature_2m_max"][i], "tmin": daily["temperature_2m_min"][i]})
        self.data = {"temp": cur["temperature_2m"], "humidity": cur["relative_humidity_2m"],
                     "wind": cur["wind_speed_10m"], "desc": desc, "kind": kind, "night": not cur.get("is_day", 1),
                     "tmax": daily["temperature_2m_max"][0], "tmin": daily["temperature_2m_min"][0],
                     "days": days, "city": self.city}
        if not self.last:
            _log(f"weather: {self.city} {round(self.data['temp'])}°, {desc}")
        self.last = time.time()

    def text(self):
        w = self.data
        if not w:
            return ""
        s = f"الان در {self.city} هوا {w['desc']} است و دما {round(w['temp'])} درجه است"
        if w.get("tmax") is not None:
            s += f"؛ بیشینه‌ی امروز {round(w['tmax'])} و کمینه {round(w['tmin'])} درجه"
        if w["days"]:
            t = w["days"][0]
            s += f". فردا {t['desc']} با بیشینه‌ی {round(t['tmax'])} درجه"
        return s + "."


# ================================================================== laptop control (Windows)
VK = {"play_pause": 0xB3, "next": 0xB0, "previous": 0xB1, "stop": 0xB2,
      "volume_up": 0xAF, "volume_down": 0xAE, "mute": 0xAD}


def press_key(name, times=1):
    import ctypes
    code = VK.get(name)
    if not code:
        return False
    for _ in range(max(1, min(int(times), 20))):
        ctypes.windll.user32.keybd_event(code, 0, 0, 0)
        ctypes.windll.user32.keybd_event(code, 0, 2, 0)
        time.sleep(0.03)
    return True


# ================================================================== everything together
class Features:
    def __init__(self, robot, cfg):
        self.r = robot                       # the Laboo object
        self.cfg = cfg or {}
        sc = self.cfg.get("screen", {}) or {}
        self.idle_after = float(sc.get("idle_minutes", 1.5)) * 60
        self.screen_on = bool(sc.get("enabled", True))
        self.persian_digits = bool(sc.get("persian_digits", True))
        self.night = sc.get("night_hours", [23, 7])
        self.bright = int(sc.get("brightness", 80))
        self.night_bright = int(sc.get("night_brightness", 15))
        self.photo_dir = HERE / sc.get("photos_folder", "photos")
        self.photo_seconds = float(sc.get("photo_seconds", 20))
        self.weather = Weather(self.cfg.get("weather", {}))
        self.apps = self.cfg.get("apps", {}) or {}
        wb = self.cfg.get("wellbeing", {}) or {}
        self.break_after = float(wb.get("break_after_minutes", 50)) * 60
        self.greet = bool(wb.get("morning_greeting", True))
        self.special = wb.get("special_days", []) or []

        self.timers = []                     # {id, end, total, label, kind}
        self.rem_file = HERE / "reminders.json"
        self.reminders = self._load_reminders()
        self.last_activity = time.time()
        self.showing = None                  # None = face, otherwise the page name on screen
        self.page = "clock"
        self.forced_until = 0
        self.last_key = None
        self.photo_i, self.photo_t = 0, 0
        self.last_break = time.time()
        self.greeted_day = None
        self.last_bl = None
        self.lock = threading.Lock()
        threading.Thread(target=self._loop, daemon=True).start()

    # -------------------------------------------------------------- state from the robot
    def activity(self):
        """Somebody talks to / touches the robot: show the face again."""
        self.last_activity = time.time()
        self.forced_until = 0
        if self.showing:
            self.showing = None
            self.last_key = None
            self.r.board.send("FACE")

    def can_draw(self):
        return self.screen_on and getattr(self.r.board, "img_ok", False)

    def is_night(self, now=None):
        h = (now or datetime.now()).hour
        a, b = self.night
        return (h >= a or h < b) if a > b else (a <= h < b)

    # -------------------------------------------------------------- board events while a picture is shown
    def on_board_event(self, ev):
        """Returns True if the event was used here.
        Face showing : swipe left/right -> weather, swipe up/down -> clock.
        Picture shown: tap -> next page (clock -> weather -> photos -> face), long press -> face."""
        if ev.startswith("img_"):
            if ev != "img_ok":
                _log(f"  (screen: {ev})")
            return True
        if not self.can_draw():
            return False
        if not self.showing:
            if ev in ("swipe_left", "swipe_right"):
                self.show_now("weather", 60)
                return True
            if ev in ("swipe_up", "swipe_down"):
                self.show_now("clock", 60)
                return True
            return False
        if self.showing == "gauge":              # gauge: long press = start/stop the demo drive (board), tap = face
            if ev.startswith("tap"):
                self.activity()
            return True
        if ev == "long":
            self.activity()
            return True
        if ev.startswith("tap") or ev.startswith("swipe"):
            pages = ["clock", "weather"] + (["photos"] if self._photos() else [])
            cur = self.showing if self.showing in pages else ("clock" if self.showing == "timer" else None)
            back = ev in ("swipe_right", "swipe_down")
            if cur is None:
                nxt = "clock"
            else:
                i = pages.index(cur) + (-1 if back else 1)
                if i >= len(pages) or i < 0:
                    self.activity()                  # past the last page -> the face again
                    return True
                nxt = pages[i]
            self.show_now(nxt, 60)
            return True
        return False

    # -------------------------------------------------------------- background loop
    def _loop(self):
        while True:
            try:
                self._tick()
            except Exception as e:
                _log(f"features error: {e!r}")
            time.sleep(0.5)

    def _tick(self):
        now = time.time()
        # timers / reminders that are due
        for t in list(self.timers):
            if now >= t["end"]:
                self.timers.remove(t)
                self.r.events.put(("alarm", {"kind": t["kind"], "label": t["label"]}))
        for rm in list(self.reminders):
            if now >= rm["at"]:
                self.reminders.remove(rm)
                self._save_reminders()
                self.r.events.put(("alarm", {"kind": "reminder", "label": rm["text"]}))
        # give the board the time once per connection (its clock chip keeps it for the car gauge)
        if self.r.board.connected.is_set():
            if not getattr(self, "time_sent", False):
                n = datetime.now()
                self.r.board.send(f"TIME {n.year} {n.month} {n.day} {n.hour} {n.minute} {n.second}")
                self.time_sent = True
        else:
            self.time_sent = False
        # backlight by time of day
        bl = self.night_bright if self.is_night() else self.bright
        if bl != self.last_bl:
            self.last_bl = bl
            self.r.board.send(f"B {bl}")
        # sitting too long
        cam = getattr(self.r, "camera", None)
        if cam and self.break_after > 0 and cam.present and getattr(cam, "present_since", 0):
            if now - cam.present_since > self.break_after and now - self.last_break > self.break_after:
                self.last_break = now
                self.r.events.put(("alarm", {"kind": "break", "label": ""}))
        # the idle screen
        if not self.can_draw() or self.showing == "gauge":
            return
        idle = self.r._idle() and now - self.last_activity > self.idle_after
        if self.forced_until and now >= self.forced_until:     # a page shown on request has had its time
            self.forced_until = 0
            self.page = "clock"
            self.last_key = None
            if not idle and self.showing:
                self.showing = None
                self.r.board.send("FACE")
        forced = now < self.forced_until
        if not (idle or forced):
            return
        self._draw(now)

    def _draw(self, now):
        import screens
        dt = datetime.now()
        night = self.is_night(dt)
        page = self.page
        running = sorted(self.timers, key=lambda t: t["end"])
        if running and page == "clock":
            page = "timer"
        if page == "timer" and running:
            t = running[0]
            rem = t["end"] - now
            key = ("timer", int(rem), night)
            if key == self.last_key:
                return
            img = screens.timer_screen(rem, t["total"], t["label"], night)
        elif page == "weather":
            key = ("weather", self.weather.last, night)
            if key == self.last_key:
                return
            img = screens.weather_screen(self.weather.data, night)
        elif page == "photos" and self._photos():
            if self.showing == "photos" and now - self.photo_t < self.photo_seconds:
                return
            ph = self._photos()
            self.photo_i = (self.photo_i + (1 if self.showing == "photos" else 0)) % len(ph)
            self.photo_t = now
            key = ("photo", self.photo_i)
            try:
                img = screens.photo_screen(ph[self.photo_i])
            except Exception as e:
                _log(f"photo {ph[self.photo_i].name}: {e}")
                return
        elif page == "message":
            return                                   # stays until touched / timeout
        else:
            nr = self.reminders[0] if self.reminders else None
            note = None
            if nr and nr["at"] - now < 3 * 3600:
                note = "یادآوری " + screens.fa_num(datetime.fromtimestamp(nr["at"]).strftime("%H:%M"))
            key = ("clock", dt.strftime("%H:%M"), self.weather.last, night, note)
            if key == self.last_key:
                return
            img = screens.clock(dt, self.weather.data, night, self.persian_digits, note)
        self.r.board.send_image(screens.encode(img))
        self.last_key = key
        self.showing = page

    def show_now(self, page, seconds=60, img=None):
        """Put something on the screen right away (e.g. 'show me the weather')."""
        if not self.can_draw():
            return False
        if img is not None:
            import screens
            self.r.board.send_image(screens.encode(img))
            self.showing = page
            self.page = page
        else:
            self.page = page
            self.last_key = None
            self._draw(time.time())
        self.forced_until = time.time() + seconds
        return True

    def show_gauge(self, demo=True):
        self.r.board.send("GAUGE DEMO" if demo else "GAUGE")
        self.showing = "gauge"
        self.last_key = None

    def _photos(self):
        if not self.photo_dir.exists():
            return []
        return sorted(p for p in self.photo_dir.iterdir() if p.suffix.lower() in (".jpg", ".jpeg", ".png", ".webp", ".bmp"))

    # -------------------------------------------------------------- timers & reminders
    def add_timer(self, seconds, label="", kind="timer"):
        seconds = max(1, int(seconds))
        t = {"id": uuid.uuid4().hex[:6], "end": time.time() + seconds, "total": seconds, "label": label or "تایمر", "kind": kind}
        self.timers.append(t)
        _log(f"  + timer {seconds}s ({t['label']})")
        return t

    def cancel_timers(self):
        n = len(self.timers)
        self.timers.clear()
        return n

    def add_reminder(self, when, text):
        self.reminders.append({"at": when.timestamp(), "text": text})
        self.reminders.sort(key=lambda r: r["at"])
        self._save_reminders()
        _log(f"  + reminder {when:%Y-%m-%d %H:%M}: {text}")

    def _load_reminders(self):
        try:
            data = json.loads(self.rem_file.read_text(encoding="utf-8"))
            now = time.time()
            return [r for r in data if r["at"] > now - 3600]
        except Exception:
            return []

    def _save_reminders(self):
        try:
            self.rem_file.write_text(json.dumps(self.reminders, ensure_ascii=False, indent=1), encoding="utf-8")
        except Exception:
            pass

    def status_text(self):
        """Told to the AI so it can answer 'how long is left?' etc."""
        now = time.time()
        parts = []
        for t in sorted(self.timers, key=lambda t: t["end"]):
            m, s = divmod(int(t["end"] - now), 60)
            parts.append(f"تایمر «{t['label']}»: {m} دقیقه و {s} ثانیه مانده")
        for r in self.reminders[:5]:
            parts.append(f"یادآوری {datetime.fromtimestamp(r['at']):%Y-%m-%d %H:%M}: {r['text']}")
        w = self.weather.text()
        if w:
            parts.append("آب‌وهوا: " + w)
        return "\n".join(parts)

    # -------------------------------------------------------------- alarms (run on the robot's main thread)
    def fire(self, info):
        r = self.r
        kind, label = info.get("kind"), info.get("label", "")
        self.activity()
        if kind == "break":
            r.board.send("E sad 6000")
            r.speaker.say("محسن، خیلی وقته پشت سیستم نشستی. پاشو یه کم راه برو، آب بخور، چشمات هم استراحت کنن!", "sad")
            return
        if kind == "greet":
            r.board.send("E happy 6000")
            r.speaker.say(label, "happy")
            return
        r.board.send("E surprised 8000")
        self.beep()
        if kind == "reminder":
            import screens
            self.show_now("message", 60, screens.message_screen("یادآوری", label, self.is_night()))
            r.speaker.say(f"یادت باشه: {label}", "surprised")
        elif kind == "pomodoro_work":
            r.speaker.say("آفرین! ۲۵ دقیقه کار تموم شد. حالا ۵ دقیقه استراحت کن.", "happy")
            self.add_timer(5 * 60, "استراحت", "pomodoro_break")
        elif kind == "pomodoro_break":
            r.speaker.say("استراحت تموم شد! بریم سراغ ۲۵ دقیقه‌ی بعدی؟ اگه آره بگو پومودورو.", "happy")
        else:
            r.speaker.say(f"تایمر {label} تموم شد!" if label and label != "تایمر" else "تایمرت تموم شد!", "surprised")
        r.board.send("S idle")

    def beep(self):
        try:
            import sounddevice as sd
            sr = 24000
            t = np.arange(int(sr * 0.18)) / sr
            tone = (np.sin(2 * np.pi * 1320 * t) * np.hanning(t.size) * 0.5)
            gap = np.zeros(int(sr * 0.12))
            snd = np.concatenate([tone, gap, tone, gap, tone, gap * 3]) .astype(np.float32)
            sd.play(snd, sr)
            sd.wait()
        except Exception:
            pass

    # -------------------------------------------------------------- morning greeting / special days
    def on_arrive(self):
        now = datetime.now()
        if not self.greet or self.greeted_day == now.date() or not (5 <= now.hour < 12):
            return False
        self.greeted_day = now.date()
        owner = (self.cfg.get("owner", {}) or {}).get("name", "")
        msg = f"صبح بخیر {owner}!".replace("  ", " ")
        w = self.weather.data
        if w:
            msg += f" امروز هوا {w['desc']} و {round(w['temp'])} درجه‌ست."
        for day in self.special:
            if self._is_today(day.get("date", "")):
                msg += " " + day.get("text", "")
        self.r.events.put(("alarm", {"kind": "greet", "label": msg}))
        return True

    @staticmethod
    def _is_today(jalali_mmdd):
        from knowledge import to_jalali
        n = datetime.now()
        _, jm, jd = to_jalali(n.year, n.month, n.day)
        return jalali_mmdd == f"{jm:02d}-{jd:02d}"

    # -------------------------------------------------------------- things said directly (no AI needed)
    def local_command(self, text):
        """Returns (reply, emotion) or None."""
        t = text.replace("ي", "ی").replace("ك", "ک")
        low = t.lower()
        if re.search(r"(هوا|آب ?و ?هوا|اب و هوا|دما)\b.*(چطور|چه ?جوری|چنده|سرد|گرم|بارون|بارونی|خوبه|امروز|فردا)|weather", low):
            w = self.weather.text()
            if not w:
                return ("الان به اطلاعات آب‌وهوا دسترسی ندارم.", "sad")
            self.show_now("weather", 60)
            return (w, "happy")
        if re.search(r"(تایمر|زنگ).*(کنسل|لغو|خاموش|پاک)|(کنسل|لغو).*(تایمر)|cancel (the )?timer", low):
            n = self.cancel_timers()
            return ("باشه، تایمر رو کنسل کردم." if n else "تایمری نداشتیم!", "neutral")
        if re.search(r"(ساعت|تاریخ).*(نشون|نمایش)|clock", low):
            self.show_now("clock", 60)
            return ("بفرما!", "happy")
        if re.search(r"(عکس|عکسا|عکس‌ها|عکسها).*(نشون|نمایش)|show.*photos", low):
            if not self._photos():
                return ("هنوز عکسی نداری! چند تا عکس بذار توی پوشه‌ی photos کنار برنامه.", "confused")
            self.show_now("photos", 300)
            return ("بفرما، اینم عکس‌ها!", "love")
        if re.search(r"گیج|گیچ|کیلومتر|سرعت ?سنج|داشبورد|حالت ماشین|ماشین (رو|را) نشون|gauge|speedometer|dashboard", low):
            self.show_gauge()
            return ("اینم گیج ماشین! انگشتت رو روی صفحه نگه دار تا ماشین خیالی راه بیفته یا وایسه.", "happy")
        if re.search(r"پومودورو|pomodoro", low):
            self.add_timer(25 * 60, "پومودورو", "pomodoro_work")
            return ("پومودورو شروع شد! ۲۵ دقیقه تمرکز، بعدش بهت می‌گم استراحت کنی.", "happy")
        return None

    # -------------------------------------------------------------- actions decided by the AI
    def do(self, a):
        """a = dict from the AI's 'action' field. Returns a short log string."""
        typ = str(a.get("type", "")).lower()
        if typ == "timer":
            secs = float(a.get("seconds") or 0) + 60 * float(a.get("minutes") or 0) + 3600 * float(a.get("hours") or 0)
            if secs > 0:
                self.add_timer(secs, a.get("label") or "تایمر")
        elif typ == "pomodoro":
            self.add_timer(25 * 60, "پومودورو", "pomodoro_work")
        elif typ == "cancel_timers":
            self.cancel_timers()
        elif typ == "reminder":
            when = self._when(a)
            if when:
                self.add_reminder(when, a.get("text") or "یادآوری")
        elif typ == "cancel_reminders":
            self.reminders.clear()
            self._save_reminders()
        elif typ == "show":
            what = a.get("what", "clock")
            if what == "face":
                self.activity()
            elif what == "gauge":
                self.show_gauge()
            else:
                self.show_now(what if what in ("clock", "weather", "photos") else "clock", 60)
        elif typ == "open":
            target = str(a.get("target", "")).strip()
            if re.match(r"^https?://", target):
                webbrowser.open(target)
            elif target.lower() in {k.lower() for k in self.apps}:
                cmd = next(v for k, v in self.apps.items() if k.lower() == target.lower())
                os.startfile(cmd) if os.path.exists(str(cmd)) else os.system(f'start "" {cmd}')
            else:
                return f"open: unknown app {target!r} (add it to config.yaml -> apps)"
        elif typ == "media":
            press_key(str(a.get("key", "")), a.get("times", 1))
        elif typ == "lock":
            import ctypes
            ctypes.windll.user32.LockWorkStation()
        else:
            return f"unknown action {a}"
        return f"action: {a}"

    @staticmethod
    def _when(a):
        now = datetime.now()
        try:
            if a.get("in_minutes"):
                return now + timedelta(minutes=float(a["in_minutes"]))
            hh, mm = [int(x) for x in str(a.get("time", "")).split(":")[:2]]
            day = datetime.strptime(a["date"], "%Y-%m-%d") if a.get("date") else now
            when = day.replace(hour=hh, minute=mm, second=0, microsecond=0)
            if when <= now and not a.get("date"):
                when += timedelta(days=1)
            return when
        except Exception:
            return None


ACTION_HELP = """action: اگر صاحبت کاری خواست که باید انجام شود، به جای null یکی از این‌ها را بگذار (و در reply کوتاه تأیید کن):
  "sleep"  (بخوابی/استراحت کنی)
  {"type": "timer", "minutes": 5, "seconds": 0, "label": "چای"}        تایمر
  {"type": "pomodoro"}                                                  پومودورو ۲۵ دقیقه‌ای
  {"type": "cancel_timers"}
  {"type": "reminder", "time": "17:30", "date": "2026-10-08" یا null, "text": "زنگ زدن به مهسا"}   یادآوری در ساعت مشخص
  {"type": "reminder", "in_minutes": 40, "text": "..."}                 یادآوری بعد از چند دقیقه
  {"type": "cancel_reminders"}
  {"type": "show", "what": "clock" | "weather" | "photos" | "gauge" | "face"}     نشان دادن روی صفحه‌ات (gauge = گیج/کیلومترشمار ماشین)
  تو یک صفحه‌ی گرد داری و می‌توانی این‌ها را نشان بدهی؛ هیچ‌وقت نگو «نمی‌توانم چیزی نشان بدهم». اگر حرفش نامفهوم بود
  (شنیدن صدا ممکن است اشتباه باشد) و به «نشان بده» ختم شد، حدس بزن کدام صفحه را می‌خواهد یا بپرس: ساعت، آب‌وهوا، عکس‌ها یا گیج؟
  {"type": "open", "target": "https://www.youtube.com/results?search_query=..."}   باز کردن سایت روی لپ‌تاپ (برای جستجو در گوگل/یوتیوب لینک جستجو بساز)
  {"type": "open", "target": "<اسم برنامه از این فهرست: {apps}>"}
  {"type": "media", "key": "play_pause" | "next" | "previous" | "volume_up" | "volume_down" | "mute", "times": 1}
  {"type": "lock"}   قفل کردن صفحه‌ی لپ‌تاپ
زمان‌ها را بر اساس «زمان الان» حساب کن. عددها را همیشه به رقم انگلیسی در JSON بنویس."""
