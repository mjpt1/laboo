# -*- coding: utf-8 -*-
"""
Local knowledge base for the robot (SQLite, file: tada.db next to this script).

  * phrases.yaml  -> sentences YOU define ("when I say X, answer Y"), loaded at every start
  * learned       -> answers Claude gave to general questions, reused next time instantly
  * taught        -> things you teach it by voice/text ("یاد بگیر وقتی گفتم ... بگو ...")
  * built-ins     -> time / date answered locally, no internet needed
"""
import difflib
import re
import sqlite3
import time
from datetime import datetime
from pathlib import Path

_FIX = str.maketrans({"ي": "ی", "ك": "ک", "‌": " ", "ـ": "", "ۀ": "ه", "ة": "ه", "أ": "ا", "إ": "ا", "آ": "ا"})
_FILLERS = {"لطفا", "لطفاً", "خب", "خوب", "ببین", "بگو", "بهم", "به", "من", "هی", "آهای", "اهای", "ای"}


def norm(text, drop=()):
    t = text.translate(_FIX).lower()
    t = re.sub(r"[ً-ٰٟ]", "", t)
    t = re.sub(r"[^\w\s]", " ", t)
    toks = [w for w in t.split() if w not in _FILLERS and w not in drop]
    return " ".join(toks)


# ------------------------------------------------------------------ Persian calendar
def to_jalali(gy, gm, gd):
    g_d_m = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334]
    gy2 = gy + 1 if gm > 2 else gy
    days = 355666 + 365 * gy + (gy2 + 3) // 4 - (gy2 + 99) // 100 + (gy2 + 399) // 400 + gd + g_d_m[gm - 1]
    jy = -1595 + 33 * (days // 12053)
    days %= 12053
    jy += 4 * (days // 1461)
    days %= 1461
    if days > 365:
        jy += (days - 1) // 365
        days = (days - 1) % 365
    jm = 1 + days // 31 if days < 186 else 7 + (days - 186) // 30
    jd = 1 + (days % 31 if days < 186 else (days - 186) % 30)
    return jy, jm, jd


J_MONTHS = ["فروردین", "اردیبهشت", "خرداد", "تیر", "مرداد", "شهریور", "مهر", "آبان", "آذر", "دی", "بهمن", "اسفند"]
WEEKDAYS = ["دوشنبه", "سه‌شنبه", "چهارشنبه", "پنج‌شنبه", "جمعه", "شنبه", "یکشنبه"]


def builtin_answer(text):
    """Answered locally: commands and questions that change every time.
    Returns (reply, emotion) or (reply, emotion, action)."""
    t = norm(text)
    now = datetime.now()
    if re.search(r"go to sleep|go to bed|sleep now|time to sleep|take a rest", t):
        return "Okay, good night! Call me when you need me.", "sleepy", "sleep"
    if re.search(r"wake up", t):
        return "I'm awake! What's up?", "happy"
    if re.search(r"what s the time|what time is it|time is it now|tell me the time", t):
        return now.strftime("It's %I:%M %p.").replace("It's 0", "It's "), "neutral"
    if re.search(r"what day is it|what s the date|today s date|what is the date|what date is it", t):
        return now.strftime("Today is %A, %B %d, %Y.").replace(" 0", " "), "happy"
    if re.search(r"(^| )(برو |بگیر |دیگه )?بخواب( دیگه)?($| )|استراحت کن|خاموش شو|برو بخوابی", t):
        return "باشه، شب بخیر! هر وقت صدام کردی بیدار می‌شم.", "sleepy", "sleep"
    if re.search(r"بیدار شو|پاشو|بلند شو", t):
        return "بیدارم! جانم؟", "happy"
    if re.search(r"ساعت (چنده|چند است|چند|رو بگو)|چه ساعتیه|ساعت چند", t):
        h, m = now.hour, now.minute
        part = "صبح" if h < 12 else ("بعد از ظهر" if h < 18 else "شب")
        h12 = h % 12 or 12
        return f"ساعت {h12} و {m} دقیقه‌ی {part} است." if m else f"ساعت {h12} {part} است.", "neutral"
    if re.search(r"امروز (چندمه|چند شنبه|چه روزیه|چندم)|تاریخ (امروز|چنده)|چه روزیه|امروز چه روزی", t):
        jy, jm, jd = to_jalali(now.year, now.month, now.day)
        return f"امروز {WEEKDAYS[now.weekday()]}، {jd} {J_MONTHS[jm - 1]} {jy} است.", "happy"
    return None


# ------------------------------------------------------------------ the database
class Knowledge:
    def __init__(self, folder, robot_name=""):
        self.folder = Path(folder)
        self.drop = set(norm(robot_name).split()) if robot_name else set()
        self.db = sqlite3.connect(str(self.folder / "tada.db"), check_same_thread=False)
        self.db.execute("""CREATE TABLE IF NOT EXISTS qa (
            id INTEGER PRIMARY KEY, q_norm TEXT, q_text TEXT, reply TEXT, emotion TEXT,
            source TEXT, hits INTEGER DEFAULT 0, created TEXT)""")
        self.db.execute("CREATE INDEX IF NOT EXISTS qa_norm ON qa(q_norm)")
        self.db.commit()
        self._rows = []
        self.load_phrases()

    # -- phrases.yaml (your own sentences) are re-imported at every start
    def load_phrases(self):
        import yaml
        f = self.folder / "phrases.yaml"
        self.db.execute("DELETE FROM qa WHERE source='manual'")
        n = 0
        if f.exists():
            data = yaml.safe_load(f.read_text(encoding="utf-8")) or []
            for item in data:
                qs = item.get("q") or []
                if isinstance(qs, str):
                    qs = [qs]
                for q in qs:
                    self._insert(q, item.get("a", ""), item.get("emotion", "happy"), "manual")
                    n += 1
        self.db.commit()
        self._reload()
        return n

    def _insert(self, q, a, emotion, source):
        qn = norm(q, self.drop)
        if not qn or not a:
            return
        self.db.execute("DELETE FROM qa WHERE q_norm=? AND source!='manual'", (qn,)) if source != "manual" else None
        self.db.execute("INSERT INTO qa(q_norm,q_text,reply,emotion,source,created) VALUES(?,?,?,?,?,?)",
                        (qn, q, a, emotion or "neutral", source, time.strftime("%Y-%m-%d %H:%M")))

    def _reload(self):
        self._rows = self.db.execute("SELECT id,q_norm,reply,emotion,source FROM qa").fetchall()

    def add(self, q, a, emotion="neutral", source="learned"):
        self._insert(q, a, emotion, source)
        self.db.commit()
        self._reload()

    def forget(self, q):
        qn = norm(q, self.drop)
        cur = self.db.execute("DELETE FROM qa WHERE q_norm=? AND source!='manual'", (qn,))
        self.db.commit()
        self._reload()
        return cur.rowcount

    def match(self, text):
        """Best stored answer for this question, or None."""
        qn = norm(text, self.drop)
        if not qn:
            return None
        best, score = None, 0.0
        for row in self._rows:
            _, rq, reply, emo, src = row
            if rq == qn:
                best, score = row, 1.0
                break
            r = difflib.SequenceMatcher(None, qn, rq).ratio()
            if r > score:
                best, score = row, r
        need = 0.88 if best and best[4] in ("manual", "taught") else 0.92
        if best and score >= need:
            self.db.execute("UPDATE qa SET hits=hits+1 WHERE id=?", (best[0],))
            self.db.commit()
            return {"reply": best[2], "emotion": best[3], "source": best[4], "score": round(score, 2)}
        return None

    def stats(self):
        return dict(self.db.execute("SELECT source, COUNT(*) FROM qa GROUP BY source").fetchall())

    def recent(self, n=10):
        return self.db.execute("SELECT source,q_text,reply FROM qa ORDER BY id DESC LIMIT ?", (n,)).fetchall()
