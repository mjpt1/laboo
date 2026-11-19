# -*- coding: utf-8 -*-
"""Plays the same Persian + English sentences with several voices so you can pick ONE
voice for both languages. The number you choose is written into config.yaml."""
import asyncio
import re
import sys
from pathlib import Path

import edge_tts
import miniaudio
import numpy as np
import sounddevice as sd

HERE = Path(__file__).parent
FA = "سلام محسن! من تادا هستم. امروز حالت چطوره؟"
EN = "Hi Mohsen! I'm Tada. How are you today?"
PITCH = "+25Hz"

VOICES = [
    ("en-US-AvaMultilingualNeural",    "female, speaks both languages"),
    ("en-US-EmmaMultilingualNeural",   "female, speaks both languages"),
    ("en-US-AndrewMultilingualNeural", "male, speaks both languages"),
    ("en-US-BrianMultilingualNeural",  "male, speaks both languages"),
    ("fr-FR-VivienneMultilingualNeural", "female, speaks both languages"),
    ("fa-IR-FaridNeural",  "current Persian voice (Persian only)"),
    ("fa-IR-DilaraNeural", "female Persian voice (Persian only)"),
    ("en-US-AnaNeural",    "current English voice (English only)"),
]


def speak(text, voice):
    async def go():
        data = bytearray()
        async for ch in edge_tts.Communicate(text, voice, pitch=PITCH).stream():
            if ch["type"] == "audio":
                data += ch["data"]
        return bytes(data)
    mp3 = asyncio.run(go())
    if not mp3:
        raise RuntimeError("no audio")
    dec = miniaudio.decode(mp3, output_format=miniaudio.SampleFormat.SIGNED16, nchannels=1, sample_rate=24000)
    sd.play(np.frombuffer(dec.samples, dtype=np.int16), 24000)
    sd.wait()


def save(voice):
    f = HERE / "config.yaml"
    t = f.read_text(encoding="utf-8")
    t = re.sub(r'(?m)^(  voice:\s*)"[^"]*"', rf'\1"{voice}"', t, count=1)
    if re.search(r'(?m)^  voice_en:', t):
        t = re.sub(r'(?m)^(  voice_en:\s*)"[^"]*"', rf'\1"{voice}"', t, count=1)
    else:
        t = re.sub(r'(?m)^(  voice:.*\n)', rf'\1  voice_en: "{voice}"\n', t, count=1)
    f.write_text(t, encoding="utf-8")
    print(f"\nSaved: voice = voice_en = {voice}.  Restart run.bat.")


def main():
    for i, (v, note) in enumerate(VOICES, 1):
        print(f"\n[{i}] {v}  ({note})")
        for text in (FA, EN):
            try:
                speak(text, v)
            except Exception as e:
                print("   could not play:", e)
                break
    print("\nWhich number should Tada use for BOTH languages?  (Enter = keep current settings)")
    for i, (v, _) in enumerate(VOICES, 1):
        print(f"  {i}. {v}")
    ans = input("> ").strip()
    if ans.isdigit() and 1 <= int(ans) <= len(VOICES):
        save(VOICES[int(ans) - 1][0])
    elif ans in ("r", "R"):
        main()
    else:
        print("Nothing changed.")


if __name__ == "__main__":
    sys.exit(main())
