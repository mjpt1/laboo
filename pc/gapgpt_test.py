# -*- coding: utf-8 -*-
"""Measures how fast each GapGPT service answers (chat, speech-to-text, voices)."""
import io
import time
import wave
from pathlib import Path

import yaml
from openai import OpenAI

HERE = Path(__file__).parent
cfg = yaml.safe_load((HERE / "config.yaml").read_text(encoding="utf-8"))
g = cfg.get("gapgpt") or {}
cli = OpenAI(api_key=g.get("api_key"), base_url=g.get("base_url") or "https://api.gapgpt.app/v1",
             timeout=40, max_retries=0)


def timed(name, fn):
    t = time.time()
    try:
        out = fn()
        print(f"OK    {name:<34} {time.time() - t:5.1f}s   {out}")
    except Exception as e:
        print(f"FAIL  {name:<34} {time.time() - t:5.1f}s   {str(e)[:150]}")


for model in [g.get("model", "claude-sonnet-5-5"), "gpt-4o-mini", "claude-haiku-4-5"]:
    timed(f"chat {model}", lambda m=model: cli.chat.completions.create(
        model=m, max_tokens=40, messages=[{"role": "user", "content": "سلام! یک جمله‌ی کوتاه بگو."}]
    ).choices[0].message.content.strip()[:60])

heard = sorted((HERE / "heard").glob("*.wav"))
if heard:
    data = heard[-1].read_bytes()
    timed(f"whisper-1 ({heard[-1].name})", lambda: cli.audio.transcriptions.create(
        model="whisper-1", file=("a.wav", data)).text[:60])
else:
    bio = io.BytesIO()
    with wave.open(bio, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(b"\0\0" * 16000)
    timed("whisper-1 (1 s of silence)", lambda: cli.audio.transcriptions.create(
        model="whisper-1", file=("a.wav", bio.getvalue())).text[:60])

text = "سلام محسن! من لبو هستم. امروز حالت چطوره؟"
timed("voice tts-1", lambda: f"{len(cli.audio.speech.create(model='tts-1', voice='nova', input=text, response_format='mp3').content)} bytes")
timed("voice gpt-4o-mini-tts", lambda: f"{len(cli.audio.speech.create(model='gpt-4o-mini-tts', voice='nova', input=text, response_format='mp3').content)} bytes")
timed("voice gpt-4o-mini-tts + style", lambda: f"{len(cli.audio.speech.create(model='gpt-4o-mini-tts', voice='nova', input=text, response_format='mp3', instructions='cheerful cute robot').content)} bytes")
