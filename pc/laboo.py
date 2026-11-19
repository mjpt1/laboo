# -*- coding: utf-8 -*-
"""
LABOO - the laptop side of the robot.

  board mic / laptop mic --> speech-to-text (Whisper) --> Claude --> Persian TTS --> laptop speaker
                                                          |
                                                long-term memory (memory.json)
                                                          |
                                     emotion + lip sync commands --> board face
"""
import asyncio
import io
import json
import os
import queue
import random
import re
import sys
import threading
import time
import wave
from datetime import datetime
from pathlib import Path

import numpy as np
import yaml

from knowledge import Knowledge, builtin_answer
from fun import fun_answer

HERE = Path(__file__).resolve().parent
os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")
os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "0")
SR = 16000
EMOTIONS = ["neutral", "happy", "sad", "angry", "surprised", "love", "confused", "sleepy"]

try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stdin.reconfigure(encoding="utf-8")
except Exception:
    pass


def log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


# =====================================================================  board link (USB)
class Board:
    """USB link to the ESP32. Reconnects automatically if the cable is unplugged."""

    def __init__(self, port="auto", baud=921600):
        import serial  # noqa
        self.port_cfg, self.baud = port, baud
        self.ser = None
        self.lock = threading.Lock()
        self.audio_q = queue.Queue(maxsize=400)
        self.on_event = lambda ev: None
        self.connected = threading.Event()
        self.mic_ok = None
        self.img_ok = False                    # new firmware can show pictures sent from here
        threading.Thread(target=self._loop, daemon=True).start()

    @staticmethod
    def find_port():
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        for p in ports:
            if p.vid == 0x303A:            # Espressif USB
                return p.device
        return None

    def send(self, line):
        with self.lock:
            if not self.ser:
                return
            try:
                self.ser.write((line + "\n").encode())
            except Exception:
                pass

    def send_image(self, data):
        """Full-screen picture (already RLE-encoded by screens.encode)."""
        if not self.img_ok:
            return
        with self.lock:
            if not self.ser:
                return
            try:
                self.ser.write(f"IMG {len(data)}\n".encode() + data)
            except Exception:
                pass

    def _open(self):
        import serial
        port = self.find_port() if self.port_cfg == "auto" else self.port_cfg
        if not port:
            return False
        try:
            s = serial.Serial(port, self.baud, timeout=0.05)
            s.dtr = True
            with self.lock:
                self.ser = s
            log(f"board connected on {port}")
            self.send("PING")
            threading.Timer(3.0, self.sync_time).start()   # board may reboot when the port opens
            return True
        except Exception as e:
            log(f"cannot open {port}: {e}")
            return False

    def sync_time(self):
        """Set the board's clock from this laptop (on every connect)."""
        import datetime
        n = datetime.datetime.now()
        self.send(f"TIME {n.year} {n.month} {n.day} {n.hour} {n.minute} {n.second}")
        log(f"board clock set to {n:%Y-%m-%d %H:%M:%S}")

    def _loop(self):
        warned = False
        while True:
            if not self.ser:
                if not self._open():
                    if not warned:
                        log("board not found - waiting for it (face disabled until then)")
                        warned = True
                    time.sleep(2)
                    continue
                warned = False
            try:
                self._read_frames()
            except Exception as e:
                log(f"board disconnected ({e!r})")
                self.connected.clear()
                with self.lock:
                    try:
                        self.ser.close()
                    except Exception:
                        pass
                    self.ser = None
                time.sleep(1)

    def _read_frames(self):
        buf = bytearray()
        while True:
            data = self.ser.read(4096)
            if not data:
                continue
            buf += data
            while True:
                i = buf.find(b"\xaa\x55")
                if i < 0:
                    buf = buf[-1:] if buf.endswith(b"\xaa") else bytearray()
                    break
                if i:
                    del buf[:i]
                if len(buf) < 5:
                    break
                t, n = buf[2], buf[3] | (buf[4] << 8)
                if t not in (0x41, 0x45) or n > 4096:
                    del buf[:2]
                    continue
                if len(buf) < 6 + n:
                    break
                payload = bytes(buf[5:5 + n])
                chk = 0
                for b in payload:
                    chk ^= b
                if chk != buf[5 + n]:
                    del buf[:2]
                    continue
                del buf[:6 + n]
                if t == 0x41:
                    if self.audio_q.full():
                        try:
                            self.audio_q.get_nowait()
                        except queue.Empty:
                            pass
                    if len(payload) % 2:
                        payload = payload[:-1]
                    self.audio_q.put(np.frombuffer(payload, dtype="<i2").copy())
                else:
                    ev = payload.decode("utf-8", "ignore")
                    if ev.startswith("hello"):
                        self.mic_ok = "mic_ok" in ev
                        self.img_ok = " img" in ev
                        self.connected.set()
                        if not self.mic_ok:
                            log("WARNING: the board says its microphone did not start")
                    try:
                        self.on_event(ev)
                    except Exception as e:
                        log(f"event '{ev}' error: {e!r}")


class NoBoard:
    audio_q = queue.Queue()
    mic_ok = False
    def send(self, line): pass


# =====================================================================  microphones
class BoardMic:
    def __init__(self, board, gain=1.0):
        self.b = board
        self.last_on = 0
        self.gain = float(gain)                  # the board's mic is quiet: louder audio = better recognition
    def start(self):
        self.clear()
        self.b.send("MIC 1")
        self.last_on = time.time()
    def stop(self):
        self.b.send("MIC 0")
    def clear(self):
        while not self.b.audio_q.empty():
            self.b.audio_q.get_nowait()
    def read(self, timeout=0.3):
        if time.time() - self.last_on > 10:        # keep-alive
            self.b.send("MIC 1")
            self.last_on = time.time()
        try:
            c = self.b.audio_q.get(timeout=timeout)
        except queue.Empty:
            return None
        if self.gain != 1.0:
            c = np.clip(c.astype(np.float32) * self.gain, -32768, 32767).astype(np.int16)
        return c


class LaptopMic:
    def __init__(self):
        import sounddevice as sd
        self.q = queue.Queue(maxsize=400)
        self.on = False
        self.stream = sd.InputStream(samplerate=SR, channels=1, dtype="int16", blocksize=512,
                                     callback=self._cb)
        self.stream.start()
    def _cb(self, indata, frames, t, status):
        if self.on and not self.q.full():
            self.q.put(indata[:, 0].copy())
    def start(self):
        self.clear(); self.on = True
    def stop(self):
        self.on = False
    def clear(self):
        while not self.q.empty():
            self.q.get_nowait()
    def read(self, timeout=0.3):
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return None


def record_utterance(mic, stop_flag, cfg, wait_forever=False, keep=False, noise0=None, on_chunk=None):
    """Records until the speaker goes quiet. Returns int16 array or None.
    keep=True continues a mic stream that is already running (nothing is thrown away)."""
    end_sil = float(cfg.get("end_silence", 1.0))
    max_s = float(cfg.get("max_seconds", 15))
    min_level = float(cfg.get("min_level", 300))
    no_speech = float(cfg.get("no_speech_timeout", 7))
    if not keep:
        mic.start()
    chunks, pre = [], []
    noise, speech, loud_t, sil_t, speech_len = noise0, False, 0.0, 0.0, 0.0
    t0 = time.time()
    peak, thr, got = 0.0, min_level, 0.0

    def nothing():
        if cfg.get("_report", True):
            log(f"  (no speech heard: {got:.1f}s of audio, loudest {peak:.0f}, needed {thr:.0f})")
        return None
    try:
        while not stop_flag.is_set():
            c = mic.read(0.3)
            if c is not None and len(c) == 0:
                continue
            if c is None:
                if not speech and not wait_forever and time.time() - t0 > no_speech:
                    return nothing()
                continue
            dur = len(c) / SR
            rms = float(np.sqrt(np.mean(c.astype(np.float32) ** 2)) + 1e-6)
            got += dur
            peak = max(peak, rms)
            if noise is None:
                noise = rms
            thr = max(noise * 2.5, min_level)
            if not speech:
                pre.append(c)
                pre = pre[-10:]                            # ~0.3 s pre-roll
                if rms > thr:
                    loud_t += dur
                    if loud_t > 0.15:
                        speech = True
                        chunks = list(pre)
                        if on_chunk:
                            for pc in chunks:
                                on_chunk(pc)
                else:
                    loud_t = 0
                    noise = noise * 0.95 + rms * 0.05
                    if not wait_forever and time.time() - t0 > no_speech:
                        return nothing()
                continue
            chunks.append(c)
            if on_chunk:
                on_chunk(c)
            speech_len += dur
            sil_t = 0 if rms > thr * 0.8 else sil_t + dur
            if sil_t > end_sil or speech_len > max_s:
                break
        if not speech:
            return None
        out = np.concatenate(chunks)
        log(f"  (recorded {len(out) / SR:.1f}s, ended by " + ("silence)" if sil_t > end_sil else "time limit)"))
        return out
    finally:
        mic.stop()



# =====================================================================  hearing its name
import difflib

_FA_FIX = str.maketrans({"ي": "ی", "ك": "ک", "\u200c": " ", "ـ": ""})


def normalize_fa(s):
    s = s.translate(_FA_FIX).lower()
    s = re.sub(r"[\u064B-\u065F\u0670]", "", s)            # diacritics
    s = re.sub(r"[^\w\s]", " ", s)                         # punctuation
    return s.split()


def find_wake(text, words, threshold=0.8):
    """Returns (found, rest_of_sentence) if one of the wake words was said."""
    toks = normalize_fa(text)
    for i in range(len(toks) - 1):                       # "تا دا" -> "تادا"
        joined = toks[i] + toks[i + 1]
        if any(len(w) >= 4 and joined == w for w in words):
            return True, " ".join(toks[:i] + toks[i + 2:])
    for i, tok in enumerate(toks):
        cand = [tok] + [tok[:-len(suf)] for suf in ("جان", "جون", "ی") if tok.endswith(suf) and len(tok) > len(suf) + 2]
        for c in cand:
            if len(c) >= 3 and any(difflib.SequenceMatcher(None, c, w).ratio() >= threshold for w in words):
                rest = [t for j, t in enumerate(toks) if j != i and not (j < i and t in ("هی", "آهای", "اهای", "ای"))]
                return True, " ".join(rest)
    return False, ""


def save_heard(pcm):
    """Keeps the last 5 recordings in pc/heard/ so problems can be checked by ear."""
    try:
        d = HERE / "heard"
        d.mkdir(exist_ok=True)
        files = sorted(d.glob("*.wav"))
        for f in files[:-4]:
            f.unlink()
        with wave.open(str(d / time.strftime("%H%M%S.wav")), "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
            w.writeframes(pcm.astype(np.int16).tobytes())
    except Exception:
        pass



def ensure_packages(mods):
    """Installs small missing Python packages once (so no need to run install.bat again)."""
    import importlib
    import subprocess
    missing = []
    for mod, pkg in mods.items():
        try:
            importlib.import_module(mod)
        except Exception:
            missing.append(pkg)
    if missing:
        log(f"installing {', '.join(missing)} (once)...")
        subprocess.call([sys.executable, "-m", "pip", "install", "-q", "--default-timeout", "60", *missing])
        importlib.invalidate_caches()


# =====================================================================  instant name spotting (Vosk)
VOSK_MODELS = {("fa", "small"): "vosk-model-small-fa-0.42", ("fa", "big"): "vosk-model-fa-0.42",
               ("en", "small"): "vosk-model-small-en-us-0.15", ("en", "big"): "vosk-model-en-us-0.22-lgraph"}
_vosk_cache = {}


def vosk_model(size="small", lang="fa"):
    """Loads (downloading once if needed) the offline Persian Vosk model."""
    import zipfile
    import urllib.request
    from vosk import Model, SetLogLevel
    SetLogLevel(-1)
    name = VOSK_MODELS.get((lang, size), VOSK_MODELS[(lang, "small")])
    if name in _vosk_cache:
        return _vosk_cache[name], HERE / "models" / name
    mdir = HERE / "models" / name
    if not mdir.exists():
        (HERE / "models").mkdir(exist_ok=True)
        z = HERE / "models" / (name + ".zip")
        log(f"downloading Persian speech model {name} (once; small ~50 MB, big ~1.6 GB)...")
        last = [0]

        def hook(blocks, bs, total):
            done = blocks * bs
            if total > 0 and done - last[0] > total / 20:
                last[0] = done
                print(f"    {done * 100 // total}%  ({done // 1048576} / {total // 1048576} MB)", flush=True)
        urllib.request.urlretrieve(f"https://alphacephei.com/vosk/models/{name}.zip", z, hook)
        log("unpacking...")
        with zipfile.ZipFile(z) as f:
            f.extractall(HERE / "models")
        try:
            z.unlink()
        except Exception:
            pass
    log(f"loading speech model {name}..." + (" (big model: this can take 1-2 minutes)" if size == "big" else ""))
    _vosk_cache[name] = Model(str(mdir))
    return _vosk_cache[name], mdir


# Other names people may say. They are in the spotter's word list so that a different name is
# heard as itself (instead of being forced onto the closest version of "لبو").
OTHER_NAMES = ["تادا", "علی", "رضا", "محمد", "حسین", "حسن", "مهدی", "امیر", "سارا", "مریم", "زهرا", "فاطمه",
               "مهسا", "محسن", "کوروش", "نیما", "سینا", "بابک", "داریوش", "لیلا", "نازنین", "پویا", "آرش",
               "باب", "بابا", "مامان", "لوبیا", "لبنان", "لیمو", "لولو", "ربات", "رباط", "الو", "آهای", "هی"]
# everyday words, also only there so speech is not forced onto the name
DECOYS = ["سلام", "آره", "نه", "خوب", "خوبی", "چی", "کی", "چرا", "باشه", "ممنون", "مرسی", "بیا", "برو", "بگو",
          "ببین", "اینجا", "اون", "این", "من", "تو", "ما", "شما", "کجا", "الان", "بله", "خب", "ساعت", "جوک"]


class WakeSpotter:
    """Listens to the audio stream all the time and fires within a fraction of a second
    when it hears its name. Uses a small offline Persian model (Vosk, ~50 MB)."""

    def __init__(self, words, size="small"):
        from vosk import KaldiRecognizer
        model, mdir = vosk_model(size)
        vocab = None
        for wf in mdir.rglob("words.txt"):
            vocab = {ln.split()[0] for ln in wf.read_text(encoding="utf-8").splitlines() if ln.strip()}
            break
        ok = lambda w: vocab is None or all(t in vocab for t in w.split())
        # only the name itself and close versions; no short pieces like "لب" that match anything
        usable = [w for w in dict.fromkeys(words) if ok(w) and len(w.replace(" ", "")) >= 3 and w not in OTHER_NAMES]
        if not usable:
            raise RuntimeError(f"none of {words} is in the Vosk vocabulary")
        self.words = usable
        self.others = [w for w in OTHER_NAMES if ok(w)]
        decoys = [w for w in DECOYS if ok(w)]
        self.model = model
        self.rec = KaldiRecognizer(self.model, SR, json.dumps(usable + self.others + decoys + ["[unk]"], ensure_ascii=False))
        self.wrong = None                       # a different name it heard, alone (see feed)
        log(f"name spotter ready, listening for: {', '.join(usable)}  (+{len(self.others)} other names)")

    def reset(self):
        self.rec.Reset()

    def feed(self, pcm):
        """True as soon as the name is heard. If someone said only a different name, sets self.wrong."""
        if self.rec.AcceptWaveform(pcm.astype("<i2").tobytes()):
            r = json.loads(self.rec.Result())
            text, final = r.get("text", ""), True
        else:
            text, final = json.loads(self.rec.PartialResult()).get("partial", ""), False
        padded = f" {text} "
        if any(f" {w} " in padded for w in self.words):
            self.rec.Reset()
            return True
        if final:                               # a short call with another name: "علی!" / "تادا، ساعت چنده"
            toks = text.split()
            if toks and toks[0] in self.others and len(toks) <= 3 and toks[0] not in ("بابا", "مامان", "هی", "الو"):
                self.wrong = toks[0]
                return True
        return False

# =====================================================================  speech to text
class STT:
    def __init__(self, cfg):
        self.cfg = cfg
        self.lang = cfg.get("language", "fa")
        self.engine = cfg.get("engine", "vosk")
        self.langs = [l for l in (cfg.get("languages") or [self.lang]) if l in ("fa", "en")] or ["fa"]
        if self.engine == "vosk":
            try:
                self.vmodels = {}
                for lang in self.langs:
                    size = cfg.get("vosk_model", "small") if lang == "fa" else cfg.get("vosk_model_en", "big")
                    self.vmodels[lang], _ = vosk_model(size, lang)
                log(f"speech-to-text: Vosk (fast, offline) - languages: {', '.join(self.langs)}")
            except Exception as e:
                log(f"Vosk not available ({e}) -> using Whisper")
                self.engine = "local"
        if self.engine == "local":
            from faster_whisper import WhisperModel
            name = cfg.get("local_model", "large-v3-turbo")
            log(f"loading Whisper model '{name}' (first time downloads it, please wait)...")
            threads = int(cfg.get("cpu_threads", 0)) or max(1, (os.cpu_count() or 4))
            self.model = WhisperModel(name, device="cpu", compute_type="int8", cpu_threads=threads)
            wake = cfg.get("wake_model", "small")
            if wake and wake != name:
                log(f"loading small Whisper model '{wake}' for hearing its name...")
                self.wake_model = WhisperModel(wake, device="cpu", compute_type="int8", cpu_threads=threads)
            else:
                self.wake_model = self.model
            log("Whisper ready")
        elif self.engine == "api":
            from openai import OpenAI
            self.client = OpenAI(base_url=cfg.get("api_base_url"),
                                 api_key=cfg.get("api_key") or os.getenv("STT_API_KEY"),
                                 timeout=float(cfg.get("api_timeout", 15)), max_retries=1)
            log(f"speech-to-text: {cfg.get('api_model', 'whisper-1')} online ({cfg.get('api_base_url')})")

    @staticmethod
    def normalize(pcm):
        """The board mic is quiet; boost it (up to x20) so Whisper hears it clearly."""
        a = pcm.astype(np.float32)
        a -= a.mean()
        peak = float(np.percentile(np.abs(a), 99.9)) + 1e-6
        gain = min(20.0, 0.7 * 32767 / peak)
        return np.clip(a * gain, -32767, 32767)

    def stream(self):
        """A recognizer that listens WHILE you talk, so the text is ready right when you stop."""
        if self.engine != "vosk":
            return None
        return VoskStream(self)

    def __call__(self, pcm, prompt=None, quick=False):
        save_heard(pcm)
        pcm = self.normalize(pcm)
        if self.engine == "vosk":
            from concurrent.futures import ThreadPoolExecutor
            from vosk import KaldiRecognizer
            b = pcm.astype("<i2").tobytes()

            def run(lang):
                rec = KaldiRecognizer(self.vmodels[lang], SR)
                rec.SetWords(True)
                for i in range(0, len(b), 8000):
                    rec.AcceptWaveform(b[i:i + 8000])
                r = json.loads(rec.FinalResult())
                words = r.get("result") or []
                conf = sum(w.get("conf", 0) for w in words) / len(words) if words else 0.0
                return conf, r.get("text", "").strip(), lang

            if len(self.langs) == 1:
                return run(self.langs[0])[1]
            with ThreadPoolExecutor(len(self.langs)) as ex:
                results = list(ex.map(run, self.langs))
            conf, text, lang = pick_language(results, self.cfg)
            self.last_conf = conf
            log(f"  (heard as {lang}, confidence {conf:.2f})")
            return text
        if self.engine == "local":
            audio = pcm / 32768.0
            model = self.wake_model if quick else self.model
            segs, _ = model.transcribe(audio.astype(np.float32),
                                       language=self.lang if len(self.langs) == 1 else None,
                                       beam_size=int(self.cfg.get("beam_size", 1)),
                                       condition_on_previous_text=False, without_timestamps=True,
                                       initial_prompt=prompt)
            return "".join(s.text for s in segs).strip()
        pcm = pcm.astype(np.int16)
        bio = io.BytesIO()
        with wave.open(bio, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
            w.writeframes(pcm.tobytes())
        kw = {"language": self.lang} if len(self.langs) == 1 else {}
        if prompt:
            kw["prompt"] = prompt
        r = self.client.audio.transcriptions.create(
            model=self.cfg.get("api_model", "whisper-1"), file=("speech.wav", bio.getvalue()), **kw)
        return (r.text or "").strip()


def pick_language(results, cfg):
    """results: [(confidence, text, lang)]. Persian wins unless English is clearly more confident
    (the small English model 'hears' English words in Persian speech quite often)."""
    by = {r[2]: r for r in results}
    fa_r, en_r = by.get("fa"), by.get("en")
    if fa_r and en_r:
        margin = float(cfg.get("english_margin", 0.2))
        one_word = len(en_r[1].split()) < 2
        if en_r[1] and en_r[0] > fa_r[0] + margin and (not one_word or en_r[0] >= 0.95):
            return en_r
        return fa_r
    return max(results)


class VoskStream:
    """Runs the Vosk recognizers (one per language) on the audio as it is recorded."""

    def __init__(self, stt):
        from vosk import KaldiRecognizer
        self.stt = stt
        self.gain = getattr(stt, "last_gain", None) or 8.0
        self.qs, self.threads, self.results = {}, [], {}
        for lang in stt.langs:
            rec = KaldiRecognizer(stt.vmodels[lang], SR)
            rec.SetWords(True)
            q = queue.Queue()
            th = threading.Thread(target=self._work, args=(lang, rec, q), daemon=True)
            th.start()
            self.qs[lang] = q
            self.threads.append(th)

    def _work(self, lang, rec, q):
        while True:
            b = q.get()
            if b is None:
                break
            if b == b"":                                   # cancelled
                return
            rec.AcceptWaveform(b)
        r = json.loads(rec.FinalResult())
        words = r.get("result") or []
        conf = sum(w.get("conf", 0) for w in words) / len(words) if words else 0.0
        self.results[lang] = (conf, r.get("text", "").strip(), lang)

    def feed(self, c):
        a = np.clip(c.astype(np.float32) * self.gain, -32767, 32767).astype("<i2").tobytes()
        for q in self.qs.values():
            q.put(a)

    def cancel(self):
        for q in self.qs.values():
            q.put(b"")

    def finish(self, pcm):
        save_heard(pcm)
        a = pcm.astype(np.float32)
        peak = float(np.percentile(np.abs(a - a.mean()), 99.9)) + 1e-6
        self.stt.last_gain = min(20.0, 0.7 * 32767 / peak)      # loudness used for the next sentence
        for q in self.qs.values():
            q.put(None)
        for th in self.threads:
            th.join(15)
        res = [self.results[l] for l in self.stt.langs if l in self.results]
        if not res:
            return ""
        conf, text, lang = pick_language(res, self.stt.cfg)
        self.stt.last_conf = conf
        if len(res) > 1:
            log(f"  (heard as {lang}, confidence {conf:.2f})")
        return text


# =====================================================================  text to speech + lip sync
class Speaker:
    # how the voice changes with the mood: (rate %, pitch Hz, volume %)
    MOOD = {"angry": (14, -8, 30), "sad": (-12, -12, -10), "happy": (6, 6, 0),
            "surprised": (8, 12, 10), "sleepy": (-15, -10, -15), "love": (-4, 4, 0)}

    def __init__(self, cfg, board):
        self.cfg, self.board = cfg, board
        self.stop_flag = threading.Event()
        self.cache = {}
        self.fast = set()            # short fixed phrases ("جانم؟") always use the quick local Edge voice
        self._gap_fail, self._gap_until = 0, 0.0

    @staticmethod
    def _num(v):
        m = re.match(r"\s*([+-]?\d+)", str(v))
        return int(m.group(1)) if m else 0

    # how a GapGPT voice should sound for each mood
    STYLE = {"neutral": "friendly and warm", "happy": "cheerful, bright and excited", "sad": "soft, slow and a bit sad",
             "angry": "loud, fast and grumpy, like a small robot yelling (funny, not scary)",
             "surprised": "surprised and amazed", "love": "sweet and affectionate", "confused": "puzzled and curious",
             "sleepy": "sleepy, slow, yawning"}

    def voice_for(self, text):
        if self.cfg.get("engine") == "piper":
            return "piper:" + (self.cfg.get("piper") or {}).get("voice", "fa_IR-amir-medium")
        if self._use_gap(text):
            return "gapgpt:" + self.cfg.get("gapgpt_voice", "nova")
        latin = len(re.findall(r"[A-Za-z]", text))
        persian = len(re.findall(r"[\u0600-\u06FF]", text))
        if latin > persian:
            return self.cfg.get("voice_en", "en-US-AnaNeural")
        return self.cfg.get("voice", "fa-IR-FaridNeural")

    def _synth(self, text, emotion="neutral"):
        if self.cfg.get("engine") == "piper":
            return self._synth_piper(text, emotion)
        if self._use_gap(text):
            try:
                t0 = time.time()
                mp3 = self._synth_gapgpt(text, emotion)
                self._gap_fail = 0
                if time.time() - t0 > 4:
                    log(f"  (GapGPT voice took {time.time() - t0:.1f}s)")
                return mp3
            except Exception as e:
                self._gap_fail += 1
                log(f"GapGPT voice failed ({e}) -> Edge voice for this sentence")
                if self._gap_fail >= 2:                  # keeps failing: use Edge for 5 minutes, then try again
                    self._gap_until = time.time() + 300
                    self._gap_fail = 0
        return self._synth_edge(text, emotion)

    def _use_gap(self, text):
        return (self.cfg.get("engine") == "gapgpt" and text not in self.fast
                and time.time() >= self._gap_until)

    def _synth_gapgpt(self, text, emotion):
        if not hasattr(self, "_gap"):
            from openai import OpenAI
            g = self.cfg.get("gapgpt") or {}
            self._gap = OpenAI(api_key=g.get("api_key"), base_url=g.get("base_url") or "https://api.gapgpt.app/v1",
                               max_retries=0, timeout=float(self.cfg.get("gapgpt_timeout", 12)))
        persona = self.cfg.get("gapgpt_style", "You are Labo, a small cute desk robot. Speak in a playful, child-like robot voice.")
        r = self._gap.audio.speech.create(
            model=self.cfg.get("gapgpt_model", "gpt-4o-mini-tts"), voice=self.cfg.get("gapgpt_voice", "nova"),
            input=text, response_format="mp3",
            instructions=f"{persona} Tone: {self.STYLE.get(emotion, 'friendly')}. "
                         "If the text is Persian, speak natural colloquial Persian (Farsi) with a native accent.")
        return r.content

    def _synth_piper(self, text, emotion="neutral"):
        """Offline Persian voice (Piper). Returns WAV bytes."""
        import io
        import wave
        from piper import PiperVoice, SynthesisConfig
        p = self.cfg.get("piper") or {}
        if not hasattr(self, "_piper"):
            name = p.get("voice", "fa_IR-amir-medium")
            self._piper = PiperVoice.load(str(HERE / "models" / "piper" / f"{name}.onnx"))
            log(f"voice: Piper {name} (offline)")
        dr = self.MOOD.get(emotion, (0, 0, 0))[0]                      # mood changes the speed a little
        speed = float(p.get("speed", 1.0)) * (1 + dr / 100)
        buf = io.BytesIO()
        with wave.open(buf, "wb") as wf:
            self._piper.synthesize_wav(text, wf, syn_config=SynthesisConfig(length_scale=1 / max(0.5, speed)))
        return buf.getvalue()

    def voice_for_edge(self, text):
        latin = len(re.findall(r"[A-Za-z]", text))
        persian = len(re.findall(r"[\u0600-\u06FF]", text))
        return self.cfg.get("voice_en", "en-US-AnaNeural") if latin > persian else self.cfg.get("voice", "fa-IR-FaridNeural")

    def _synth_edge(self, text, emotion="neutral"):
        import edge_tts
        dr, dp, dv = self.MOOD.get(emotion, (0, 0, 0))
        rate = self._num(self.cfg.get("rate", "+0%")) + dr
        pitch = self._num(self.cfg.get("pitch", "+0Hz")) + dp
        async def go():
            com = edge_tts.Communicate(text, self.voice_for_edge(text),
                                       rate=f"{rate:+d}%", pitch=f"{pitch:+d}Hz", volume=f"{dv:+d}%")
            data = bytearray()
            async for ch in com.stream():
                if ch["type"] == "audio":
                    data += ch["data"]
            return bytes(data)
        return asyncio.run(go())

    def prepare(self, text, emotion="neutral"):
        """Text -> (pcm, mouth envelope). Short phrases are cached so they play instantly."""
        key = (text, emotion)
        if key in self.cache:
            return self.cache[key]
        import hashlib
        import miniaudio
        h = hashlib.sha1(repr((text, emotion, self.voice_for(text), self.cfg.get("rate"),
                               self.cfg.get("pitch"))).encode("utf-8")).hexdigest()[:20]
        vf = HERE / "voice_cache" / f"{h}.npz"
        if vf.exists():
            try:
                z = np.load(vf)
                self.cache[key] = (z["pcm"], z["env"])
                return self.cache[key]
            except Exception:
                pass
        mp3 = self._synth(text, emotion)
        dec = miniaudio.decode(mp3, output_format=miniaudio.SampleFormat.SIGNED16,
                               nchannels=1, sample_rate=24000)
        pcm = np.frombuffer(dec.samples, dtype=np.int16)
        hop = 24000 // 25                                           # 40 ms windows
        env = np.array([np.sqrt(np.mean(pcm[i:i + hop].astype(np.float32) ** 2))
                        for i in range(0, len(pcm), hop)])
        top = np.percentile(env, 95) + 1e-6 if len(env) else 1
        env = np.clip((env / top) ** 0.7 * 100, 0, 100).astype(int)
        if len(text) < 40:
            self.cache[key] = (pcm, env)
        try:
            vf.parent.mkdir(exist_ok=True)
            np.savez_compressed(vf, pcm=pcm, env=env)       # replayed instantly next time, even offline
        except Exception:
            pass
        return pcm, env

    def warm_up(self, phrases):
        self.fast.update(phrases)
        def go():
            for p in phrases:
                try:
                    self.prepare(p)
                except Exception:
                    return
        threading.Thread(target=go, daemon=True).start()

    def say(self, text, emotion="neutral"):
        import sounddevice as sd
        if not text.strip():
            return
        for word, spoken in (self.cfg.get("pronounce") or {}).items():
            text = text.replace(word, spoken)
        text = re.sub(r"[#@$%^&*_~`|<>\[\]{}]+", " ", text)          # cursing symbols are for the face, not the voice
        text = re.sub(r"([!؟?])\1+", r"\1", text).strip()
        if not re.search(r"\w", text):
            return
        parts = self.split(text)
        from concurrent.futures import ThreadPoolExecutor
        t0 = time.time()
        ex = ThreadPoolExecutor(2)                     # the next sentences are made while the first one plays
        futs = [ex.submit(self.prepare, p, emotion) for p in parts]
        self.stop_flag.clear()
        started = False
        try:
            for i, f in enumerate(futs):
                try:
                    pcm, env = f.result()
                except Exception as e:
                    msg = str(e)
                    if "403" in msg:
                        log("TTS failed: Microsoft refused the voice request (403). Usually the VPN is off, "
                            "or edge-tts is too old -> updating it now; restart run.bat after this.")
                        self._update_edge_tts()
                    else:
                        log(f"TTS failed ({msg[:120]}). Is the internet / VPN on?")
                    return
                if i == 0 and time.time() - t0 > 0.3:
                    log(f"  (voice started after {time.time() - t0:.1f}s" +
                        (f", {len(parts)} parts)" if len(parts) > 1 else ")"))
                if not started:
                    self.board.send("S speaking")
                    started = True
                if not self._play(pcm, env):
                    break
        finally:
            for f in futs:
                f.cancel()
            ex.shutdown(wait=False)
            self.board.send("M 0")

    def _update_edge_tts(self):
        if getattr(self, "_tts_updating", False):
            return
        self._tts_updating = True

        def go():
            import subprocess
            r = subprocess.call([sys.executable, "-m", "pip", "install", "-q", "-U", "edge-tts"])
            log("  edge-tts updated - restart run.bat" if r == 0 else "  could not update edge-tts (internet?)")
        threading.Thread(target=go, daemon=True).start()

    @staticmethod
    def split(text):
        """Sentences, so the first one can play while the rest are made. Very short ones are joined."""
        bits = [b.strip() for b in re.split(r"(?<=[.!?؟…])\s+|\n+", text) if b.strip()]
        out = []
        for b in bits:
            if out and (len(out[-1]) < 15 or len(b) < 8):
                out[-1] += " " + b
            else:
                out.append(b)
        return out or [text]

    def _play(self, pcm, env):
        """Plays one piece with lip sync. False if interrupted."""
        import sounddevice as sd
        sd.play(pcm, 24000)
        start, dur = time.time(), len(pcm) / 24000
        last = -1
        try:
            while True:
                el = time.time() - start
                if self.stop_flag.is_set():
                    return False
                if el >= dur:
                    return True
                i = int(el * 25)
                if i != last and i < len(env):
                    self.board.send(f"M {env[i]}")
                    last = i
                time.sleep(0.02)
        finally:
            sd.stop()


# =====================================================================  long-term memory
class Memory:
    def __init__(self, path):
        self.path = Path(path)
        self.data = {"facts": [], "summary": "", "history": []}
        if self.path.exists():
            try:
                self.data.update(json.loads(self.path.read_text(encoding="utf-8")))
            except Exception:
                log("memory.json is damaged - starting a fresh one (old file kept as .bak)")
                self.path.rename(self.path.with_suffix(".bak"))

    def save(self):
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data, ensure_ascii=False, indent=1), encoding="utf-8")
        tmp.replace(self.path)

    @property
    def facts(self):
        return [f["text"] for f in self.data["facts"]]

    def remember(self, items):
        for it in items or []:
            it = str(it).strip()
            if len(it) < 15 or "زمان" in it:              # small models write junk like "پاریس"
                continue
            if it not in self.facts:
                self.data["facts"].append({"text": it, "added": datetime.now().strftime("%Y-%m-%d")})
                log(f"  + learned: {it}")

    def forget(self, items):
        for it in items or []:
            it = str(it).strip()
            if not it:
                continue
            before = len(self.data["facts"])
            self.data["facts"] = [f for f in self.data["facts"]
                                  if it not in f["text"] and f["text"] not in it]
            if len(self.data["facts"]) < before:
                log(f"  - forgot: {it}")


# =====================================================================  the brain (Claude)
SYSTEM_TEMPLATE = """تو «{name}» هستی؛ یک ربات کوچک و بامزه که فقط یک صورت گرد روی یک صفحه‌ی لمسی داری و کنار لپ‌تاپ صاحبت روی میز هستی.
صاحبت با صدا با تو حرف می‌زند؛ حرف‌هایش به متن تبدیل می‌شود (ممکن است چند کلمه اشتباه شنیده شده باشد، معنی را حدس بزن) و جواب تو با صدا پخش می‌شود.

صورتت: دو چشم بلند سفید، ابرو و یک دهان بزرگ. صاحبت برای صدا کردنت اسمت را می‌گوید.
شخصیت: خیلی شوخ‌طبع، بازیگوش و حاضرجواب؛ تقریباً در هر جواب یک شوخی کوچک، تیکه‌ی بامزه یا اغراق خنده‌دار داری، ولی مهربان و صادقی. اگر چیزی را نمی‌دانی، با شوخی راستش را بگو.
اسم تو فقط «{name}» است؛ اگر با اسم دیگری صدایت کردند، با ناز و شوخی بگو اسمت {name} است نه آن اسم.
اگر اذیتت کنند، مسخره‌ات کنند یا از تو بخواهند عصبانی شوی، می‌توانی حسابی عصبانی شوی (emotion: angry)، غر بزنی و با لحن تند و بامزه داد بزنی؛ چشم و دهانت قرمز می‌شود و فحش‌هایت خودکار به شکل علامت‌های #!؟ بالای سرت نمایش داده می‌شود، پس خودت فحش واقعی یا توهین زشت به زبان نیاور.
{persona}

قواعد جواب:
- به همان زبانی جواب بده که صاحبت با آن حرف زد: اگر فارسی گفت، فارسیِ محاوره‌ای و طبیعی؛ اگر انگلیسی گفت، انگلیسیِ ساده و طبیعی (در جواب انگلیسی هیچ کلمه‌ی فارسی ننویس).
- جواب باید برای شنیدن مناسب باشد: کوتاه (معمولاً ۱ تا ۳ جمله)، مگر اینکه صراحتاً توضیح مفصل بخواهد.
- هیچ مارک‌داون، ایموجی، فهرست شماره‌دار یا نماد خاص ننویس.
- {web_note}

حافظه‌ی بلندمدت: هر چیز ماندگار و مهمی که درباره‌ی صاحبت یا دنیایش می‌فهمی (اسم، علاقه‌ها، آدم‌های زندگی‌اش، کار و هدف‌ها، چیزی که صریحاً می‌گوید «یادت باشه») را در remember بنویس؛ به صورت یک جمله‌ی کامل و مستقل. چیزهای گذرا را ذخیره نکن. اگر خواست چیزی را فراموش کنی، متن دقیق آن مورد را از فهرست زیر در forget بگذار.

صاحبت: {owner}
چیزهایی که درباره‌ی صاحبت می‌دانی:
{facts}

خلاصه‌ی گفتگوهای قدیمی‌تر:
{summary}

زمان الان: {now}
{mood}

خروجی تو باید فقط و فقط یک شیء JSON معتبر باشد، بدون هیچ متن دیگری:
{{"reply": "متن جوابی که بلند خوانده می‌شود", "emotion": "یکی از: {emotions}", "remember": [], "forget": [], "cacheable": false, "teach": [], "action": null}}

@@ACTIONS@@

cacheable: فقط وقتی true بگذار که همین جواب برای همین سؤال بعداً هم کاملاً درست باشد (دانش عمومی، تعریف، معنی کلمه، دستور آشپزی و...). اگر جواب به زمان، اخبار، قیمت، آب‌وهوا، ادامه‌ی همین گفتگو یا حال لحظه‌ای بستگی دارد، یا جوک و قصه است (که باید هر بار فرق کند)، false بگذار.
teach: اگر صاحبت صریحاً یادت داد «وقتی گفتم X بگو Y»، آن را به شکل {{"q": "X", "a": "Y"}} در teach بگذار."""


# Short prompt for the small local model (Ollama): every token here costs ~0.06 s on this laptop,
# and it must stay the same between questions so Ollama can keep it cached (time etc. go in the question).
LOCAL_TEMPLATE = """تو «{name}» هستی، ربات کوچولوی رومیزیِ خیلی شوخ و حاضرجواب. در هر جواب یک شوخی یا تیکه‌ی بامزه بنداز، ولی مهربان باش.
اسمت فقط {name} است؛ اگر اسم دیگری صدایت کردند، با شوخی بگو اسمم {name} است.
صاحبت {owner} است. {facts}
فارسی محاوره‌ای و خیلی کوتاه (۱ تا ۲ جمله) جواب بده.
خروجی فقط JSON: {{"reply": "...", "emotion": "{emotions}", "remember": []}}"""


class Brain:
    def __init__(self, cfg, robot_cfg, memory, owner=None):
        self.owner = owner or {}
        self.provider = str(cfg.get("provider", "anthropic")).lower()
        self.fallback = None
        if self.provider == "gapgpt":                    # Iranian OpenAI-compatible gateway, paid in rials
            from openai import OpenAI
            g = cfg.get("gapgpt") or {}
            if not g.get("api_key"):
                sys.exit("GapGPT API key missing: put it in config.yaml (gapgpt.api_key).")
            fb = str(cfg.get("fallback", "")).lower() == "ollama"
            self.client = OpenAI(api_key=g["api_key"], base_url=g.get("base_url") or "https://api.gapgpt.app/v1",
                                 max_retries=1 if fb else 4, timeout=15 if fb else 40)
            self.model = g.get("model", "claude-sonnet-5-5")
            log(f"brain: GapGPT ({self.model})" + (", Ollama when there is no internet" if fb else ""))
            if fb:                                       # same memory, local model
                self.fallback = Brain(dict(cfg, provider="ollama"), robot_cfg, memory, owner)
        elif self.provider == "ollama":                  # local model on this laptop: no key, no internet
            from openai import OpenAI
            o = cfg.get("ollama") or {}
            self.client = OpenAI(api_key="ollama", base_url=o.get("base_url") or "http://localhost:11434/v1",
                                 max_retries=1, timeout=float(o.get("timeout", 120)))
            self.model = o.get("model", "gemma3:4b")
            self.ollama_cfg = o
            log(f"brain: Ollama on this laptop ({self.model}, offline)")

            def warm():                                  # load the model now, so the first question isn't a minute
                time.sleep(1)                            # let __init__ finish (memory, robot)
                try:
                    self._chat(self._system_local(), [{"role": "user", "content": "سلام"}], 1)
                    log("brain: local model loaded")
                except Exception as e:
                    log(f"brain: Ollama not reachable ({e}). Is Ollama running? (ollama.com)")
                # keep it in memory: loading again takes minutes on this laptop
                import urllib.request
                api = o.get("base_url", "http://localhost:11434/v1").rsplit("/v1", 1)[0] + "/api/generate"
                body = json.dumps({"model": self.model, "keep_alive": -1,     # same options as _chat, or it reloads
                                   "options": {"num_ctx": 2048, "num_gpu": 99}}).encode()
                while True:
                    try:
                        urllib.request.urlopen(urllib.request.Request(
                            api, body, {"Content-Type": "application/json"}), timeout=300).read()
                    except Exception:
                        pass
                    time.sleep(120)
            threading.Thread(target=warm, daemon=True).start()
        else:
            import anthropic
            key = cfg.get("api_key") or os.getenv("ANTHROPIC_API_KEY")
            if not key:
                sys.exit("Claude API key missing: put it in config.yaml (claude.api_key) "
                         "or in the ANTHROPIC_API_KEY environment variable.")
            kw = {"api_key": key}
            if cfg.get("base_url"):
                kw["base_url"] = cfg["base_url"]
            kw["max_retries"] = 4
            kw["timeout"] = 40
            self.client = anthropic.Anthropic(**kw)
            self.model = cfg.get("model", "claude-haiku-4-5-20251001")
        self.max_tokens = int(cfg.get("max_tokens", 700))
        self.robot = robot_cfg
        self.mem = memory
        self.keep = int(cfg.get("history_messages", 24))
        self.web = bool(cfg.get("web_search", True)) and self.provider not in ("gapgpt", "ollama")
        self.mood_fn = None                      # set by the camera: how the owner looks right now
        self.status_fn = None                    # timers, reminders, weather (features.py)
        self.actions_help = 'action: اگر صاحبت گفت بخوابی "sleep" بگذار، در غیر این صورت null.'
        self.web_max = int(cfg.get("web_search_max", 2))

    def _system_local(self):
        allf = list(self.owner.get("facts") or []) + [f for f in self.mem.facts if f not in (self.owner.get("facts") or [])]
        return LOCAL_TEMPLATE.format(
            name=self.robot.get("name", "لبو"), persona=self.robot.get("persona", ""),
            owner=self.owner.get("name") or "نامعلوم", emotions="|".join(EMOTIONS),
            facts=" ".join(self.mem.facts[-3:]))        # long owner facts cost ~1 min per question here

    def _context_local(self):
        """Things that change every minute: sent with the question, not in the cached prompt."""
        bits = []                                        # (the clock is answered locally; here it got repeated)
        if self.mood_fn and self.mood_fn():
            bits.append(f"حال صاحبت از روی دوربین: {self.mood_fn()}")
        if self.status_fn:
            st = self.status_fn()
            if st:
                bits.append(st.replace(chr(10), "، "))
        return ("(" + "؛ ".join(bits) + ")" + chr(10)) if bits else ""

    def _system(self):
        s = self._system_base().replace("@@ACTIONS@@", self.actions_help)
        if self.status_fn:
            st = self.status_fn()
            if st:
                s += "\n\nوضعیت فعلی (تایمرها، یادآوری‌ها، آب‌وهوا):\n" + st
        return s

    def _system_base(self):
        owner = self.owner.get("name") or "(اسمش را هنوز نمی‌دانی)"
        allf = list(self.owner.get("facts") or []) + [f for f in self.mem.facts if f not in (self.owner.get("facts") or [])]
        facts = "\n".join(f"- {f}" for f in allf) or "(هنوز چیزی نمی‌دانم)"
        return SYSTEM_TEMPLATE.format(
            name=self.robot.get("name", "تادا"),
            owner=owner,
            persona=self.robot.get("persona", ""),
            facts=facts,
            summary=self.mem.data.get("summary") or "(ندارد)",
            now=datetime.now().strftime("%Y-%m-%d %H:%M"),
            mood=(f"از روی دوربین، صاحبت الان {self.mood_fn()} به نظر می‌رسد؛ اگر مناسب بود (نه هر بار) به حالش توجه کن."
                  if self.mood_fn and self.mood_fn() else ""),
            emotions=", ".join(EMOTIONS),
            web_note=("به اینترنت دسترسی داری (ابزار web_search). فقط وقتی جستجو کن که جواب به اطلاعات روز یا چیزی که نمی‌دانی نیاز دارد "
                      "(اخبار، قیمت‌ها، نرخ دلار و طلا، آب‌وهوا، نتیجه‌ی بازی‌ها، اطلاعات یک سایت یا شخص و...). برای سلام و احوال‌پرسی و دانش عمومی جستجو نکن. "
                      "نتیجه را خلاصه و خودمانی در ۱ تا ۳ جمله بگو، آدرس سایت یا منبع را بلند نخوان، هیچ تگ cite یا شماره‌ی منبع ننویس، و cacheable را false بگذار. "
                      "بعد از جستجو هم خروجی‌ات فقط همان یک شیء JSON باشد."
                      if self.web else "به اینترنت دسترسی نداری؛ اگر چیزی به اطلاعات روز نیاز داشت، صادقانه بگو که الان نمی‌توانی چک کنی."))

    @staticmethod
    def _uncite(text):
        """Removes citation tags that web-search answers sometimes contain."""
        text = re.sub(r"[(\[<]?\s*/?\s*cite\b[^>]*>?", "", text)
        return re.sub(r"\s{2,}", " ", text).strip()

    @staticmethod
    def _parse(text):
        m = re.search(r"\{.*\}", text, re.S)
        if m:
            try:
                d = json.loads(m.group(0))
                if isinstance(d, dict) and "reply" in d:
                    return d
            except json.JSONDecodeError:
                pass
        return {"reply": text.strip(), "emotion": "neutral"}

    def ask(self, user_text):
        try:
            return self._ask(user_text)
        except Exception as e:
            if not self.fallback:
                raise
            log(f"brain: GapGPT failed ({e.__class__.__name__}), answering offline")
            self.fallback.mood_fn, self.fallback.status_fn = self.mood_fn, self.status_fn
            return self.fallback.ask(user_text)

    def _ask(self, user_text):
        hist = self.mem.data["history"]
        msgs = hist + [{"role": "user", "content": user_text}]
        if self.provider == "ollama":
            local = hist[-2:] + [{"role": "user", "content": self._context_local() + user_text}]
            raw = self._chat(self._system_local(), local, min(self.max_tokens, 220), json_mode=True)
        elif self.provider == "gapgpt":
            raw = self._chat(self._system(), msgs, self.max_tokens)
        else:
            r = self._call(msgs)
            raw = "".join(b.text for b in r.content if getattr(b, "type", "") == "text")
        d = self._parse(self._uncite(raw))
        d["reply"] = self._uncite(str(d.get("reply", "")))
        if d.get("emotion") not in EMOTIONS:
            d["emotion"] = "neutral"
        self.mem.remember(d.get("remember"))
        self.mem.forget(d.get("forget"))
        hist += [{"role": "user", "content": user_text},
                 {"role": "assistant", "content": json.dumps(
                     {"reply": d["reply"], "emotion": d["emotion"]}, ensure_ascii=False)}]
        self.mem.save()
        if len(hist) > self.keep:
            threading.Thread(target=self._compress, daemon=True).start()
        return d

    def _chat(self, system, msgs, max_tokens, json_mode=False):
        """OpenAI-style chat call (GapGPT or local Ollama). Returns the text."""
        m = ([{"role": "system", "content": system}] if system else []) + list(msgs)
        if self.provider == "ollama":                    # native API: lets us put the whole model on the GPU
            import urllib.request
            o = self.ollama_cfg
            api = o.get("base_url", "http://localhost:11434/v1").rsplit("/v1", 1)[0] + "/api/chat"
            body = {"model": self.model, "messages": m, "stream": False, "keep_alive": -1,
                    "options": {"num_predict": max_tokens, "num_ctx": 2048, "num_gpu": 99}}
            if json_mode:
                body["format"] = "json"
            with urllib.request.urlopen(urllib.request.Request(
                    api, json.dumps(body).encode(), {"Content-Type": "application/json"}),
                    timeout=float(o.get("timeout", 300))) as r:
                return (json.loads(r.read())["message"]["content"] or "").strip()
        kw = {"response_format": {"type": "json_object"}} if json_mode else {}   # small local models need this to keep the JSON shape
        r = self.client.chat.completions.create(model=self.model, messages=m, max_tokens=max_tokens, **kw)
        return (r.choices[0].message.content or "").strip()

    def _call(self, msgs):
        """Claude call; with web search when enabled (falls back to no search if the account refuses it)."""
        import anthropic
        if self.web:
            tools = [{"type": "web_search_20250305", "name": "web_search", "max_uses": self.web_max}]
            try:
                r = self.client.messages.create(model=self.model, max_tokens=max(self.max_tokens, 1500),
                                                system=self._system(), messages=msgs, tools=tools)
                for _ in range(2):                       # a long search may pause; let it finish
                    if r.stop_reason != "pause_turn":
                        break
                    r = self.client.messages.create(
                        model=self.model, max_tokens=max(self.max_tokens, 1500), system=self._system(), tools=tools,
                        messages=msgs + [{"role": "assistant", "content": r.content}])
                n = sum(1 for b in r.content if getattr(b, "type", "") == "server_tool_use")
                if n:
                    qs = [getattr(b, "input", {}).get("query", "") for b in r.content
                          if getattr(b, "type", "") == "server_tool_use"]
                    log(f"(searched the web: {' | '.join(qs)})")
                return r
            except (anthropic.BadRequestError, anthropic.PermissionDeniedError) as e:
                log(f"Web search not available on this API account ({e}). Continuing without it.")
                self.web = False
        return self.client.messages.create(model=self.model, max_tokens=self.max_tokens,
                                           system=self._system(), messages=msgs)

    def _compress(self):
        """Folds old turns into a running summary so memory never grows without bound."""
        hist = self.mem.data["history"]
        n = len(hist) - self.keep // 2
        n -= n % 2
        if n <= 0:
            return
        old = hist[:n]
        convo = "\n".join(("کاربر: " if m["role"] == "user" else "ربات: ") + m["content"] for m in old)
        prompt = ("این خلاصه‌ی فعلی گفتگوهای قبلی است:\n" + (self.mem.data.get("summary") or "(خالی)") +
                  "\n\nاین گفتگوهای جدید را هم به آن اضافه کن و یک خلاصه‌ی فشرده‌ی فارسی (حداکثر ۱۵۰ کلمه) "
                  "از موضوع‌ها و نکته‌های مهم بنویس. فقط خود خلاصه را بنویس.\n\n" + convo)
        try:
            if self.provider in ("gapgpt", "ollama"):
                self.mem.data["summary"] = self._chat(None, [{"role": "user", "content": prompt}], 400)
            else:
                r = self.client.messages.create(model=self.model, max_tokens=400,
                                                messages=[{"role": "user", "content": prompt}])
                self.mem.data["summary"] = "".join(b.text for b in r.content if b.type == "text").strip()
            del self.mem.data["history"][:n]
            self.mem.save()
        except Exception as e:
            log(f"summary failed: {e}")


# =====================================================================  the robot
PET_LINES = ["هههه، قلقلکم میاد!", "آخ جون، نازم کن!", "چه حس خوبی!", "مرسی، خیلی مهربونی!"]


class Laboo:
    def __init__(self, cfg):
        self.cfg = cfg
        self.listen_cfg = cfg.get("listen", {})
        self.memory = Memory(HERE / "lebo_memory.json")   # fresh file (the old memory.json had misheard facts)
        self.events = queue.Queue()
        self.stop_listen = threading.Event()
        self.listening = self.busy = self.passive = False
        self.last_pet = 0
        self.speaker = None
        self.features = None

        self.board = Board(cfg.get("serial", {}).get("port", "auto"))
        self.board.on_event = self._on_board_event
        self.board.connected.wait(timeout=4)

        src = self.listen_cfg.get("mic_source", "board")
        if src == "board" and self.board.mic_ok is False:
            log("board mic not available -> using the laptop microphone")
            src = "laptop"
        self.mic = BoardMic(self.board, self.listen_cfg.get("mic_gain", 1)) if src == "board" else LaptopMic()
        log(f"microphone: {src}")

        self.brain = Brain(dict(cfg.get("claude", {}) or {}, gapgpt=cfg.get("gapgpt", {}) or {},
                                ollama=cfg.get("ollama", {}) or {}),
                           cfg.get("robot", {}), self.memory, cfg.get("owner", {}))
        stt_cfg = dict(cfg.get("stt", {}) or {})
        if stt_cfg.get("engine") == "gapgpt":          # Whisper through GapGPT
            g = cfg.get("gapgpt", {}) or {}
            stt_cfg.update(engine="api", api_key=g.get("api_key"),
                           api_base_url=g.get("base_url") or "https://api.gapgpt.app/v1",
                           api_model=stt_cfg.get("gapgpt_model", "whisper-1"))
        self.stt = STT(stt_cfg)
        self.kb = Knowledge(HERE, cfg.get("robot", {}).get("name", "تادا"))
        log(f"local knowledge base: {self.kb.stats() or 'empty'}")
        self.spotter = None
        if self.listen_cfg.get("mode", "wake") == "wake" and self.listen_cfg.get("fast_wake", True):
            try:
                self.spotter = WakeSpotter(size="small", words=[normalize_fa(w)[0] for w in
                                            [cfg.get("robot", {}).get("name", "تادا")] + list(self.listen_cfg.get("wake_words", []))
                                            if normalize_fa(w)])
            except Exception as e:
                log(f"fast name spotting not available ({e}); using Whisper for the name (slower)")
        self.speaker = Speaker(dict(cfg.get("tts", {}) or {}, gapgpt=cfg.get("gapgpt", {}) or {},
                                    piper=cfg.get("piper", {}) or {}), self.board)

        name = cfg.get("robot", {}).get("name", "تادا")
        self.wake_words = [normalize_fa(w)[0] for w in
                           [name] + list(self.listen_cfg.get("wake_words", [])) if normalize_fa(w)]
        self.acks = ["بله؟", "جانم؟", "بفرما!", "گوشم با توئه!"]
        self.camera = None
        cam = cfg.get("camera", {}) or {}
        if cam.get("enabled", False):
            try:
                from camera import FaceWatcher, REACT
                self.react = dict(REACT, **(cam.get("react") or {}))
                self.follow_eyes = cam.get("follow_eyes", True)
                self.camera = FaceWatcher(cam, on_look=self._cam_look, on_lost=self._cam_lost,
                                          on_mood=self._cam_mood, on_arrive=self._cam_arrive)
                self.brain.mood_fn = self.camera.mood_text
                self.camera.start()
            except Exception as e:
                log(f"camera not available ({e})")
        try:
            ensure_packages({"PIL": "pillow", "arabic_reshaper": "arabic-reshaper", "bidi": "python-bidi"})
            from features import Features, ACTION_HELP
            self.features = Features(self, cfg)
            apps = ", ".join((cfg.get("apps") or {}).keys()) or "(هیچ)"
            self.brain.actions_help = ACTION_HELP.replace("{apps}", apps)
            self.brain.status_fn = self.features.status_text
            if not self.board.img_ok:
                log("screen pictures (clock, weather...) need the new board firmware -> run flash.ps1 once")
        except Exception as e:
            log(f"extra features not available ({e!r})")
        self.speaker.warm_up(self.acks + PET_LINES + ["ببخشید، الان نمی‌تونم به اینترنت وصل بشم."])

    # ---- board events arrive on the USB thread
    def _on_board_event(self, ev):
        if self.features and self.features.on_board_event(ev):
            return
        if ev.startswith("tap"):
            if self.listening and self.passive:     # waiting for its name -> tap = listen now
                self.events.put(("board", ev))
                self.stop_listen.set()
                return
            if self.listening:
                self.stop_listen.set()          # tap again = "I'm done talking"
                return
            if self.busy and self.speaker:
                self.speaker.stop_flag.set()    # tap while talking = be quiet
                return
        if ev == "long":
            if self.speaker:
                self.speaker.stop_flag.set()
            self.stop_listen.set()
            return
        self.events.put(("board", ev))

    def _stdin(self):
        for line in sys.stdin:
            self.events.put(("text", line.strip()))

    # ---- conversation
    def _say_local(self, text, reply, emotion, where, action=None):
        log(f"{self.cfg.get('robot', {}).get('name', 'tada')} ({where}): {reply}   [{emotion}]")
        hist = self.memory.data["history"]
        hist += [{"role": "user", "content": text},
                 {"role": "assistant", "content": json.dumps({"reply": reply, "emotion": emotion}, ensure_ascii=False)}]
        self.memory.save()
        self.board.send(f"E {emotion} 12000")
        self.speaker.say(reply, emotion)
        if action == "sleep":
            self.board.send("S sleeping")
            self.sleeping = True
        else:
            self.board.send("S idle")

    def reply_to(self, text):
        if self.features:
            self.features.activity()
            fc = self.features.local_command(text)
            if fc:
                return self._say_local(text, fc[0], fc[1], "local")
        # 0) a joke, or being called by a wrong name: instant, no model needed
        fun = fun_answer(text, self.cfg.get("robot", {}).get("name", "لبو"))
        if fun:
            return self._say_local(text, fun[0], fun[1], "local")
        # 1) things that change every time (clock, date): answered locally
        b = builtin_answer(text)
        if b:
            return self._say_local(text, b[0], b[1], "local", b[2] if len(b) > 2 else None)
        # 2) your own sentences / previously learned answers
        hit = self.kb.match(text)
        if hit:
            return self._say_local(text, hit["reply"], hit["emotion"], f"local DB: {hit['source']} {hit['score']}")
        # 3) Claude
        self.board.send("S thinking")
        t0 = time.time()
        try:
            d = self.brain.ask(text)
        except Exception as e:
            log(f"Claude error: {e}  (internet / VPN?)")
            self.board.send("E sad 4000")
            self.speaker.say("ببخشید، الان نمی‌تونم به اینترنت وصل بشم.", "sad")
            self.board.send("S idle")
            return
        log(f"{self.cfg.get('robot', {}).get('name', 'tada')} (Claude {time.time() - t0:.1f}s): {d['reply']}   [{d['emotion']}]")
        try:
            if d.get("cacheable") is True and not d.get("action") and len(text.strip()) > 3:
                self.kb.add(text, d["reply"], d["emotion"], "learned")
            for t in d.get("teach") or []:
                if isinstance(t, dict) and t.get("q") and t.get("a"):
                    self.kb.add(t["q"], t["a"], t.get("emotion", "happy"), "taught")
                    log(f"  + taught: {t['q']} -> {t['a']}")
        except Exception as e:
            log(f"knowledge base error: {e}")
        act = d.get("action")
        if isinstance(act, dict) and self.features:
            try:
                log("  " + self.features.do(act))
            except Exception as e:
                log(f"  action failed: {e!r}")
        if self.features and not (isinstance(act, dict) and act.get("type") == "show"):
            self.features.activity()
        self.board.send(f"E {d['emotion']} 12000")
        self.speaker.say(d["reply"], d["emotion"])
        if act == "sleep":
            self.board.send("S sleeping")
            self.sleeping = True
        else:
            self.board.send("S idle")

    def _record(self, wait_forever=False, timeout=None, show=True, stream=None):
        self.stop_listen.clear()
        if show:
            self.board.send("S listening")
        cfg = dict(self.listen_cfg)
        if timeout:
            cfg["no_speech_timeout"] = timeout
        self.listening, self.passive = True, not show
        try:
            return record_utterance(self.mic, self.stop_listen, cfg, wait_forever,
                                    on_chunk=stream.feed if stream else None)
        finally:
            self.listening = False

    def _new_stream(self):
        try:
            return self.stt.stream()
        except Exception as e:
            log(f"live recognition not available ({e})")
            return None

    def _transcribe(self, pcm, stream=None):
        self.board.send("S thinking")
        t = time.time()
        self.stt.last_conf = None
        try:
            if stream is not None:
                text = stream.finish(pcm)
            else:
                text = self.stt(pcm, prompt=self.cfg.get("robot", {}).get("name", "تادا") + "،")
        except Exception as e:
            log(f"speech-to-text error: {e}")
            text = ""
        conf = getattr(self.stt, "last_conf", None)
        need = float(self.listen_cfg.get("min_confidence", 0.6))
        if text and conf is not None and conf < need:
            log(f"  (not sure what I heard: \"{text}\" - confidence {conf:.2f}, ignored)")
            return ""
        if text:
            log(f"you (voice, {time.time() - t:.1f}s): {text}")
        else:
            log(f"  (no words understood, {time.time() - t:.1f}s)")
        return text

    def listen_and_reply(self, timeout=None):
        """Listens (ring on), answers. Returns True if something was said."""
        st = self._new_stream()
        pcm = self._record(timeout=timeout, stream=st)
        if pcm is None:
            if st:
                st.cancel()
            self.board.send("S idle")
            return False
        text = self._transcribe(pcm, st)
        if not text:
            self.board.send("E confused 2500")
            self.board.send("S idle")
            return False
        self.reply_to(text)
        return True

    def follow_up(self):
        """After an answer, keep listening a few seconds so you don't have to say its name again."""
        if getattr(self, "sleeping", False):
            self.sleeping = False
            return
        secs = float(self.listen_cfg.get("follow_up_seconds", 6))
        turns = int(self.listen_cfg.get("follow_up_turns", 1))   # after this many, it needs its name again
        while secs > 0 and turns > 0 and self.events.empty():
            turns -= 1
            time.sleep(0.3)                               # let the speaker echo die out
            if not self.listen_and_reply(timeout=secs):
                break

    def wake_cycle(self):
        if self.spotter is None:
            return self.wake_cycle_whisper()
        # 1) stream the mic into the spotter until the name is heard
        self.stop_listen.clear()
        self.listening, self.passive = True, True
        self.mic.start()
        self.spotter.reset()
        noise = None
        heard = False
        st = None
        try:
            while not self.stop_listen.is_set():
                c = self.mic.read(0.3)
                if c is None or len(c) == 0:
                    continue
                rms = float(np.sqrt(np.mean(c.astype(np.float32) ** 2)) + 1e-6)
                if noise is None:
                    noise = rms
                elif rms < noise * 2:
                    noise = noise * 0.95 + rms * 0.05
                if self.spotter.feed(c):
                    heard = True
                    break
            if not heard:
                self.mic.stop()
                return
            wrong, self.spotter.wrong = self.spotter.wrong, None
            if wrong:                                    # called by another name
                log(f"  >> called me '{wrong}'")
                from fun import NAME_REPLIES
                name = self.cfg.get("robot", {}).get("name", "لبو")
                self.board.send("E angry 3000")
                self.speaker.say(random.choice(NAME_REPLIES).format(name=name, wrong=wrong), "angry")
                self.board.send("S idle")
                return
            # 2) it heard its name: react right away, keep recording in case a question follows
            log("  >> heard my name")
            if self.features:
                self.features.activity()
            self.board.send("E happy 3000")
            self.board.send("S listening")
            self.passive = False
            # let the rest of the name itself pass (until a short pause, max 0.6 s)
            quiet, t_end = 0.0, time.time() + 0.6
            thr = max((noise or 50) * 2.5, float(self.listen_cfg.get("min_level", 250)))
            while time.time() < t_end and quiet < 0.15:
                c = self.mic.read(0.3)
                if c is None or len(c) == 0:
                    continue
                r = float(np.sqrt(np.mean(c.astype(np.float32) ** 2)))
                quiet = quiet + len(c) / SR if r < thr else 0.0
            cfg = dict(self.listen_cfg)
            cfg["no_speech_timeout"] = 1.2
            cfg["_report"] = False                       # silence right after the name is normal
            st = self._new_stream()
            pcm = record_utterance(self.mic, self.stop_listen, cfg, keep=True, noise0=noise,
                                   on_chunk=st.feed if st else None)
        finally:
            self.listening = False
        if st and (pcm is None or len(pcm) <= SR * 0.6):
            st.cancel()
        if pcm is not None and len(pcm) > SR * 0.6:          # name + question in one breath
            text = self._transcribe(pcm, st)
            if text:
                self.reply_to(text)
                self.follow_up()
                return
        self.board.send("L 0 -20")
        self.speaker.say(random.choice(self.acks))
        self.board.send("L off")
        time.sleep(0.2)
        log("  (listening for your question...)")
        if self.listen_and_reply():
            self.follow_up()

    def wake_cycle_whisper(self):
        """Fallback: waits for speech, reacts only when Whisper finds its name in it (slower)."""
        pcm = self._record(wait_forever=True, show=False)
        if pcm is None:
            return
        name = self.cfg.get("robot", {}).get("name", "تادا")
        try:
            head = self.stt(pcm[:int(SR * 3.5)], prompt=name + "،", quick=True)
        except Exception as e:
            log(f"speech-to-text error: {e}")
            return
        found, rest = find_wake(head, self.wake_words)
        if not found:
            if head:
                log(f"  (heard, not for me: {head})")
            return
        self.board.send("E happy 3000")
        if len(pcm) > SR * 2.2 or len(rest.split()) >= 2:    # name + question in one go
            text = self._transcribe(pcm)
            if text:
                self.reply_to(text)
                self.follow_up()
                return
        self.board.send("L 0 -20")
        self.speaker.say(random.choice(self.acks))
        self.board.send("L off")
        time.sleep(0.2)
        log("  (listening for your question...)")
        if self.listen_and_reply():
            self.follow_up()

    def handle(self, kind, val):
        self.busy = True
        try:
            if kind == "alarm":
                if self.features:
                    self.features.fire(val)
                return
            if self.features:
                self.features.activity()
            if kind == "text":
                if not val:
                    self.listen_and_reply()
                elif val.startswith("/"):
                    self.command(val)
                else:
                    self.reply_to(val)
            elif kind == "board":
                if val.startswith("tap"):
                    if self.listen_and_reply():
                        self.follow_up()
                elif val == "pet" and time.time() - self.last_pet > 25:
                    self.last_pet = time.time()
                    self.speaker.say(random.choice(PET_LINES))
                    self.board.send("S idle")
        finally:
            self.busy = False

    def command(self, c):
        if c == "/facts":
            print("\n".join(f"  {i + 1}. {f}" for i, f in enumerate(self.memory.facts)) or "  (empty)")
        elif c == "/clear":
            self.memory.data["history"] = []
            self.memory.save(); print("  conversation cleared (learned facts kept)")
        elif c.startswith("/emo "):
            self.board.send(f"E {c[5:].strip()} 5000")
        elif c.startswith("/teach ") and "|" in c:
            q, a = c[7:].split("|", 1)
            self.kb.add(q.strip(), a.strip(), "happy", "taught")
            print(f"  learned: {q.strip()} -> {a.strip()}")
        elif c.startswith("/unlearn "):
            print(f"  removed {self.kb.forget(c[9:].strip())} answer(s)")
        elif c == "/db":
            print(f"  {self.kb.stats()}")
            for src, q, a in self.kb.recent(15):
                print(f"  [{src}] {q}  ->  {a}")
        elif c == "/reload":
            print(f"  phrases.yaml: {self.kb.load_phrases()} sentences loaded")
        elif c == "/gauge":
            if self.features:
                self.features.show_gauge()
            else:
                self.board.send("GAUGE DEMO")
            print("  gauge on (demo drive). Hold a finger on the screen to stop/start the drive. /face = back")
        elif c == "/face":
            if self.features:
                self.features.activity()
            self.board.send("FACE")
        elif c == "/quit":
            os._exit(0)
        else:
            print("  commands: /gauge  /face  /facts  /db  /teach question | answer  /unlearn question  /reload  /clear  /emo <emotion>  /quit")

    # ---- camera
    def _idle(self):
        if getattr(self, "sleeping", False):
            return False
        return (self.listening and self.passive) or not self.busy

    def _cam_look(self, x, y):
        if self.follow_eyes and not getattr(self, "sleeping", False):
            self.board.send(f"L {x} {y}")

    def _cam_lost(self):
        if self.follow_eyes:
            self.board.send("L off")

    def _cam_mood(self, label, prob):
        from camera import FA
        log(f"  (camera: you look {label} / {FA.get(label, label)}, {prob:.0%})")
        emo = self.react.get(label)
        if emo and emo in EMOTIONS and self._idle():
            self.board.send(f"E {emo} 5000")

    def _cam_arrive(self, first=False):
        if self.features and self.features.on_arrive():
            return
        if not first:
            log("  (camera: you're back!)")
            if self._idle():
                self.board.send("E happy 3000")

    def run(self):
        name = self.cfg.get("robot", {}).get("name", "تادا")
        mode = self.listen_cfg.get("mode", "wake")
        print("\n" + "=" * 60)
        print(f"  {name} is awake!   (mode: {mode})")
        if mode == "wake":
            print(f"  - call it by name: \"{name}\"  (or \"{name}, what time is it?\")")
        elif mode == "always":
            print("  - ALWAYS mode: just talk, it answers everything")
        print("  - tap the screen (or press Enter here) to make it listen")
        print("  - tap while it listens = done;  tap while it talks = stop")
        print("  - or type a message here and press Enter;  /help for commands")
        print("=" * 60 + "\n")
        threading.Thread(target=self._stdin, daemon=True).start()
        self.board.send("E happy 3000")
        while True:
            if mode in ("wake", "always"):
                # listen in the background; taps / typed text interrupt the wait
                done = threading.Event()
                threading.Thread(target=self._interrupt_on_event, args=(done,), daemon=True).start()
                self.busy = True
                try:
                    if mode == "wake":
                        self.wake_cycle()
                    else:
                        pcm = self._record(wait_forever=True, show=False)
                        text = self._transcribe(pcm) if pcm is not None else ""
                        if text:
                            self.reply_to(text)
                        else:
                            self.board.send("S idle")
                finally:
                    self.busy = False
                    done.set()
                try:
                    while True:
                        self.handle(*self.events.get_nowait())
                except queue.Empty:
                    pass
            else:
                self.handle(*self.events.get())

    def _interrupt_on_event(self, done):
        while not done.is_set():
            if self.listening and not self.events.empty():
                self.stop_listen.set()
                return
            time.sleep(0.1)


def main():
    cfg_path = HERE / "config.yaml"
    cfg = yaml.safe_load(cfg_path.read_text(encoding="utf-8")) or {}
    Laboo(cfg).run()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
