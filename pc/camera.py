# -*- coding: utf-8 -*-
"""
Tada's eyes: watches the laptop camera, follows your face with its eyes and reads your
facial expression (happy / sad / surprised / angry ...). Everything stays on the laptop:
no picture is saved or sent anywhere.

  * face finding   : YuNet (face_detection_yunet_2023mar.onnx, 230 KB, in pc/models/)
  * expression     : FER+ model (emotion-ferplus-8.onnx, ~35 MB, in pc/models/)

Run this file directly (camera_test.bat) to see a preview window with what it detects.
"""
import threading
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).parent
MODEL = HERE / "models" / "emotion-ferplus-8.onnx"
MODEL_URL = ("https://media.githubusercontent.com/media/onnx/models/main/validated/vision/"
             "body_analysis/emotion_ferplus/model/emotion-ferplus-8.onnx")
YUNET = HERE / "models" / "face_detection_yunet_2023mar.onnx"
YUNET_URL = ("https://media.githubusercontent.com/media/opencv/opencv_zoo/main/models/"
             "face_detection_yunet/face_detection_yunet_2023mar.onnx")
LABELS = ["neutral", "happiness", "surprise", "sadness", "anger", "disgust", "fear", "contempt"]
FA = {"neutral": "معمولی", "happiness": "خوشحال", "surprise": "متعجب", "sadness": "ناراحت",
      "anger": "عصبانی", "disgust": "بیزار", "fear": "نگران", "contempt": "بی‌حوصله"}
# what Tada's face does when it sees yours (can be changed in config.yaml -> camera.react)
REACT = {"happiness": "happy", "sadness": "sad", "surprise": "surprised", "anger": "confused",
         "fear": "surprised", "disgust": "confused", "contempt": "confused"}


def _log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


def ensure_model():
    """The model is shipped in two parts (file size limit); join them, or download once."""
    if MODEL.exists() and MODEL.stat().st_size > 30_000_000:
        return MODEL
    MODEL.parent.mkdir(exist_ok=True)
    parts = sorted(MODEL.parent.glob(MODEL.name + ".part*"))
    if parts:
        with open(MODEL, "wb") as out:
            for p in parts:
                out.write(p.read_bytes())
        if MODEL.stat().st_size > 30_000_000:
            for p in parts:
                try:
                    p.unlink()
                except Exception:
                    pass
            return MODEL
    import urllib.request
    _log("downloading the face-expression model (~35 MB, once)...")
    urllib.request.urlretrieve(MODEL_URL, MODEL)
    return MODEL


class FaceReader:
    """Finds the biggest face in a frame and reads its expression."""

    def __init__(self):
        import cv2
        self.cv2 = cv2
        self.yunet = self.haar = None
        if hasattr(cv2, "FaceDetectorYN"):               # OpenCV 4.8+ and 5.x
            if not YUNET.exists():
                import urllib.request
                YUNET.parent.mkdir(exist_ok=True)
                _log("downloading the face-finder model (230 KB, once)...")
                urllib.request.urlretrieve(YUNET_URL, YUNET)
            self.yunet = cv2.FaceDetectorYN.create(str(YUNET), "", (320, 240), 0.7, 0.3, 50)
        elif hasattr(cv2, "CascadeClassifier"):          # old OpenCV
            self.haar = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
        else:
            raise RuntimeError("this OpenCV has no face finder")
        self.net = cv2.dnn.readNetFromONNX(str(ensure_model()))

    def face(self, frame):
        """frame: BGR camera picture. Returns (x, y, w, h) of the biggest face, or None."""
        H, W = frame.shape[:2]
        k = W / 320
        small = self.cv2.resize(frame, (320, int(H / k)))
        if self.yunet is not None:
            self.yunet.setInputSize((small.shape[1], small.shape[0]))
            _, faces = self.yunet.detect(small)
            if faces is None or len(faces) == 0:
                return None
            f = max(faces, key=lambda f: f[2] * f[3])
            x, y, w, h = (float(v) for v in f[:4])
        else:
            g = self.cv2.cvtColor(small, self.cv2.COLOR_BGR2GRAY)
            faces = self.haar.detectMultiScale(g, scaleFactor=1.15, minNeighbors=5, minSize=(40, 40))
            if len(faces) == 0:
                return None
            x, y, w, h = max(faces, key=lambda f: f[2] * f[3])
        x, y = max(0.0, x), max(0.0, y)
        return int(x * k), int(y * k), int(w * k), int(h * k)

    def expression(self, gray, box):
        x, y, w, h = box
        m = int(w * 0.1)
        crop = gray[max(0, y - m):y + h + m, max(0, x - m):x + w + m]
        if crop.size == 0:
            return None
        crop = self.cv2.resize(crop, (64, 64)).astype(np.float32)
        self.net.setInput(crop.reshape(1, 1, 64, 64))
        s = self.net.forward()[0]
        e = np.exp(s - s.max())
        return e / e.sum()


class MoodJudge:
    """Turns the model's raw scores into a mood, compared with YOUR normal face.
    A frown only raises 'anger'/'sadness' a little above how your face usually looks, so instead of
    waiting for them to beat 'neutral', it learns your resting face and reacts to the rise."""
    NEG = [4, 3, 5]                                     # anger, sadness, disgust (frowning)

    def __init__(self, need=0.55, rise=0.12):
        self.need, self.rise = need, rise
        self.base = np.array([0.85, 0.02, 0.01, 0.04, 0.07, 0.005, 0.005, 0.01])   # typical resting face
        self.ema, self.t, self.last, self.t0 = None, None, None, None

    def reset(self):
        self.ema = None

    def update(self, p, now=None):
        now = now or time.time()
        dt = 0.3 if self.t is None else min(2.0, now - self.t)
        self.t = now
        if self.t0 is None:
            self.t0 = now
        self.ema = p if self.ema is None else self.ema * 0.6 + p * 0.4
        lab, prob = self._judge(self.ema)
        # learn how YOUR face normally looks: quickly at the start, then slowly
        # (also slowly while "frowning", so a resting face that just looks a bit stern stops counting)
        tau = 4.0 if now - self.t0 < 15 else (20.0 if lab == "neutral" else 120.0)
        if lab in ("neutral", "anger", "sadness", "disgust"):
            a = min(1.0, dt / tau)
            self.base = self.base * (1 - a) + self.ema * a
        if now - self.t0 < 15:
            return "neutral", float(self.ema[0])         # still learning your face
        return lab, prob

    def _judge(self, e):
        i = int(np.argmax(e))
        if i != 0 and e[i] >= self.need:                 # clear expression (smile, surprise...)
            return LABELS[i], float(e[i])
        d = e - self.base
        neg = sum(max(0.0, d[k]) for k in self.NEG)
        if neg >= self.rise:                             # frown: anger/sadness clearly above your normal
            j = max(self.NEG, key=lambda k: d[k])
            if self.last in self.NEG and d[self.last] > d[j] - 0.05:
                j = self.last                            # don't flicker between anger and sadness
            self.last = j
            return LABELS[j], float(neg)
        for k in (2, 6, 7):                              # surprise, fear, contempt
            if d[k] >= self.rise * 1.5:
                return LABELS[k], float(d[k])
        return "neutral", float(e[0])


class FaceWatcher(threading.Thread):
    """Background thread. Calls on_look(x, y) / on_lost() to move the eyes,
    on_mood(label, prob) when your expression clearly changes, on_arrive() when you come back."""

    def __init__(self, cfg, on_look=None, on_lost=None, on_mood=None, on_arrive=None):
        super().__init__(daemon=True)
        self.cfg = cfg or {}
        self.on_look, self.on_lost, self.on_mood, self.on_arrive = on_look, on_lost, on_mood, on_arrive
        self.mood, self.mood_prob, self.mood_time = "neutral", 0.0, 0.0
        self.present, self.last_seen = False, 0.0
        self.present_since = 0.0
        self.running = True

    def mood_text(self, max_age=90):
        """For Claude: how you look right now (empty if unknown / neutral / old)."""
        if not self.present or self.mood == "neutral" or time.time() - self.mood_time > max_age:
            return ""
        return FA.get(self.mood, self.mood)

    def run(self):
        try:
            import cv2
            cv2.setNumThreads(int(self.cfg.get("cpu_threads", 1)))   # leave the CPU free for listening
            reader = FaceReader()
        except Exception as e:
            _log(f"camera off: {e}  (run install.bat again to add opencv-python)")
            return
        idx = int(self.cfg.get("index", 0))
        cap = cv2.VideoCapture(idx, cv2.CAP_DSHOW) if hasattr(cv2, "CAP_DSHOW") else cv2.VideoCapture(idx)
        if not cap.isOpened():
            cap = cv2.VideoCapture(idx)
        if not cap.isOpened():
            _log(f"camera {idx} could not be opened (in use by another app? try camera.index: 1)")
            return
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        _log("camera on: following your face and reading your expression")
        flip = -1 if self.cfg.get("mirror", True) else 1
        fps = float(self.cfg.get("fps", 6))
        need = float(self.cfg.get("mood_threshold", 0.55))
        hold = float(self.cfg.get("mood_hold_seconds", 1.5))
        away = float(self.cfg.get("away_minutes", 5)) * 60
        judge = MoodJudge(need, float(self.cfg.get("frown_sensitivity", 0.12)))
        cand, cand_t, look, n = None, 0.0, (999, 999), 0
        while self.running:
            t0 = time.time()
            ok, frame = cap.read()
            if not ok:
                time.sleep(0.5)
                continue
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            box = reader.face(frame)
            now = time.time()
            if box is None:
                if self.present and now - self.last_seen > 2.0:
                    self.present = False
                    judge.reset()
                    if self.on_lost:
                        self.on_lost()
            else:
                if not self.present:
                    first = not self.last_seen
                    if (first or now - self.last_seen > away) and self.on_arrive:
                        self.on_arrive(first)
                    self.present = True
                    self.present_since = now
                self.last_seen = now
                x, y, w, h = box
                H, W = gray.shape
                lx = int(flip * ((x + w / 2) / W * 2 - 1) * 100)
                ly = int(((y + h / 2) / H * 2 - 1) * 70)
                if abs(lx - look[0]) + abs(ly - look[1]) > 8 and self.on_look:
                    look = (lx, ly)
                    self.on_look(lx, ly)
                n += 1
                if n % 2 == 0:                                  # expression every other frame
                    p = reader.expression(gray, box)
                    if p is not None:
                        lab, prob = judge.update(p, now)
                        if lab != cand:
                            cand, cand_t = lab, now
                        elif now - cand_t >= hold and lab != self.mood:
                            self.mood, self.mood_prob, self.mood_time = lab, prob, now
                            if self.on_mood:
                                self.on_mood(lab, prob)
                        if lab == self.mood:
                            self.mood_time = now
            time.sleep(max(0.0, 1.0 / fps - (time.time() - t0)))
        cap.release()


def preview(index=0):
    """camera_test.bat: a window showing the face box and the expression it reads."""
    import cv2
    reader = FaceReader()
    cap = cv2.VideoCapture(index, cv2.CAP_DSHOW) if hasattr(cv2, "CAP_DSHOW") else cv2.VideoCapture(index)
    if not cap.isOpened():
        print(f"camera {index} could not be opened")
        return
    print("Press Q (in the window) to quit.  Look normal for ~10 s first so it learns your resting face.")
    judge, last = MoodJudge(), ""
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        box = reader.face(frame)
        if box:
            x, y, w, h = box
            p = reader.expression(gray, box)
            lab, prob = judge.update(p)
            col = (0, 255, 0) if lab == "neutral" else (0, 0, 255)
            cv2.rectangle(frame, (x, y), (x + w, y + h), col, 2)
            cv2.putText(frame, lab, (x, max(20, y - 10)), cv2.FONT_HERSHEY_SIMPLEX, 0.9, col, 2)
            if lab != last:
                print(f"--> {lab}")
                last = lab
            for k in np.argsort(-p)[:3]:
                print(f"{LABELS[k]:>10} {p[k]:.0%}", end="  ")
            print()
        cv2.imshow("Tada camera test (Q = quit)", frame)
        if cv2.waitKey(1) & 0xFF in (ord("q"), ord("Q"), 27):
            break
    cap.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    import sys
    preview(int(sys.argv[1]) if len(sys.argv) > 1 else 0)
